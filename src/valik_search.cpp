#include <future>

#include <valik/search/env_var_pack.hpp>
#include <valik/search/load_index.hpp>
#include <valik/search/query_record.hpp>
#include <valik/search/search_time_statistics.hpp>
#include <valik/search/prefilter_queries_parallel.hpp>
#include <valik/split/reference_metadata.hpp>
#include <valik/split/reference_segments.hpp>
#include <utilities/external_process.hpp>

#include <raptor/threshold/threshold.hpp>

#include <seqan3/io/sequence_file/output.hpp>

#include <stellar/database_id_map.hpp>
#include <stellar/diagnostics/print.tpp>
#include <stellar/stellar_launcher.hpp>
#include <stellar/stellar_output.hpp>
#include <stellar/io/import_sequence.hpp>
#include <stellar/utils/stellar_app_runtime.hpp>
#include <stellar3.shared.hpp>

namespace valik::app
{

//-----------------------------
//
// Setup IBF and launch multithreaded search.
//
//-----------------------------
template <bool compressed>
bool run_program(search_arguments const &arguments, search_time_statistics & time_statistics)
{
    using index_structure_t = std::conditional_t<compressed, index_structure::ibf_compressed, index_structure::ibf>;
    auto index = valik_index<index_structure_t>{};

    bool error_triggered = false; // indicates if an error happen inside this function

    {
        auto start = std::chrono::high_resolution_clock::now();
        load_index(index, arguments.index_file);
        auto end = std::chrono::high_resolution_clock::now();
        time_statistics.index_io_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    }

    using fields = seqan3::fields<seqan3::field::id, seqan3::field::seq>;
    using types = seqan3::type_list<std::string, std::vector<seqan3::dna4>>;
    using sequence_record_type = seqan3::sequence_record<types, fields>;

    seqan3::sequence_file_input<dna4_traits, fields> fin{arguments.query_file};
    std::vector<query_record> query_records{};

    raptor::threshold::threshold const thresholder{arguments.make_threshold_parameters()};

    env_var_pack var_pack{};
    sync_out synced_out{arguments.out_file};
    auto queue = cart_queue<query_record>{index.ibf().bin_count(), arguments.cart_max_capacity, arguments.max_queued_carts};

    std::optional<reference_segments> segments;
    if (!arguments.seg_path.empty())
        segments = reference_segments(arguments.seg_path);

    std::optional<reference_metadata> ref_meta;
    if (!arguments.ref_meta_path.empty())
        ref_meta = reference_metadata(arguments.ref_meta_path, false);

    //!WORKAROUND: Stellar does not allow smaller error rates
    double er_rate = std::max((double) arguments.errors / (double) arguments.pattern_size, 0.00001);

    std::mutex mutex;
    std::unordered_map<size_t, size_t> bin_count;

    struct LocalData {
        std::vector<std::string> output_files;
        std::stringstream        text_out;
        std::vector<double>      timeStatistics;
        std::vector<stellar::stellar_app_runtime> stellarTimes;
    };

    std::vector<LocalData> localData(arguments.threads);

    using TAlphabet = seqan2::Dna;
    using TSequence = seqan2::String<TAlphabet>;
    seqan2::StringSet<TSequence> databases;
    seqan2::StringSet<seqan2::CharString> databaseIDs;
    using TSize = decltype(length(databases[0]));
    TSize refLen;

    if (arguments.shared_memory)
    {
        stellar::stellar_app_runtime stellarTime{};

        for (auto bin_paths : index.bin_path())
        {
            for (auto path : bin_paths)
            {
                bool const databasesSuccess = stellarTime.input_databases_time.measure_time([&]()
                {
                    std::cout << "Launching stellar search on a shared memory machine...\n";
                    return stellar::_importAllSequences(path.c_str(), "database", databases, databaseIDs, refLen, std::cout, std::cerr);
                });
                if (!databasesSuccess)
                    return false;
            }
        }
    }

    auto consumerThreads = std::vector<std::jthread>{};
    for (size_t threadNbr = 0; threadNbr < arguments.threads; ++threadNbr)
    {
        consumerThreads.emplace_back(
        [&, threadNbr]() {
            auto& ld = localData[threadNbr];
            // this will block until producer threads have added carts to queue
            for (auto next = queue.dequeue(); next; next = queue.dequeue())
            {
                auto & [bin_id, records] = *next;

                std::unique_lock g(mutex);
                std::filesystem::path cart_queries_path = var_pack.tmp_path / std::string("query_" + std::to_string(bin_id) +
                                                          "_" + std::to_string(bin_count[bin_id]++) + ".fasta");
                g.unlock();

                ld.output_files.push_back(cart_queries_path.string() + ".gff");

                {
                    seqan3::sequence_file_output fout{cart_queries_path, fields{}};

                    for (auto & record : records)
                    {
                        sequence_record_type sequence_record{std::move(record.sequence_id), std::move(record.sequence)};
                        fout.push_back(sequence_record);
                    }
                }

                if (arguments.shared_memory)
                {
                    stellar::StellarOptions threadOptions{};
                    stellar::stellar_app_runtime stellarThreadTime{};
                    using TDatabaseSegment = stellar::StellarDatabaseSegment<TAlphabet>;

                    // import query sequences
                    seqan2::StringSet<TSequence> queries;
                    seqan2::StringSet<seqan2::CharString> queryIDs;

                    using TSize = decltype(length(queries[0]));
                    TSize queryLen{0};   // does not get populated currently
                    //!TODO: split query sequence
                    bool const queriesSuccess = stellarThreadTime.input_queries_time.measure_time([&]()
                    {
                        return stellar::_importAllSequences(cart_queries_path.c_str(), "query", queries, queryIDs, queryLen, ld.text_out, ld.text_out);
                    });
                    if (!queriesSuccess)
                    {
                        std::cerr << "Error importing queries\n";
                        error_triggered = true;
                    }

                    threadOptions.queryFile = cart_queries_path.string();
                    threadOptions.prefilteredSearch = true;
                    threadOptions.referenceLength = refLen;
                    if (segments && ref_meta)
                    {
                        threadOptions.searchSegment = true;
                        auto seg = segments->segment_from_bin(bin_id);
                        threadOptions.binSequences.emplace_back(seg.ref_ind);
                        threadOptions.segmentBegin = seg.start;
                        threadOptions.segmentEnd = seg.start + seg.len;
                    }
                    else
                    {
                        if (index.bin_path().size() < (size_t) bin_id) {
                            throw std::runtime_error("Could not find reference file with index " + std::to_string(bin_id) +
                            ". Did you forget to provide metadata to search segments in a single reference file instead?");
                        }
                        threadOptions.binSequences.push_back(bin_id);   //!TODO: what if mutliple sequence files per bin
                    }
                    threadOptions.numEpsilon = er_rate;
                    threadOptions.epsilon = stellar::utils::fraction::from_double(threadOptions.numEpsilon).limit_denominator();
                    threadOptions.minLength = arguments.pattern_size;
                    threadOptions.outputFile = cart_queries_path.string() + ".gff";
                    stellar::_writeFileNames(threadOptions, ld.text_out);
                    stellar::_writeSpecifiedParams(threadOptions, ld.text_out);
                    stellar::_writeCalculatedParams(threadOptions, ld.text_out);   // calculate qGram
                    ld.text_out << std::endl;
                    stellar::_writeMoreCalculatedParams(threadOptions, threadOptions.referenceLength, queries, ld.text_out);


                    auto current_time = stellarThreadTime.swift_index_construction_time.now();
                    stellar::StellarIndex<TAlphabet> stellarIndex{queries, threadOptions};
                    stellar::StellarSwiftPattern<TAlphabet> swiftPattern = stellarIndex.createSwiftPattern();

                    // Construct index of the queries
                    ld.text_out << "Constructing index..." << '\n';
                    stellarIndex.construct();
                    ld.text_out << std::endl;
                    stellarThreadTime.swift_index_construction_time.manual_timing(current_time);

                    auto databaseSegment = stellar::_getDREAMDatabaseSegment<TAlphabet, TDatabaseSegment>
                                        (databases[threadOptions.binSequences[0]], threadOptions);

                    //!TODO: these are passed as const, could be shared by all threads
                    stellar::DatabaseIDMap<TAlphabet> databaseIDMap{databases, databaseIDs};
                    size_t const databaseRecordID = databaseIDMap.recordID(databaseSegment);
                    seqan2::CharString const & databaseID = databaseIDMap.databaseID(databaseRecordID);

                    //!TODO: process disabled queries
                    std::vector<size_t> disabledQueryIDs{};

                    //this is shared by forward and reverse strands by mergeIn()
                    stellar::StellarOutputStatistics outputStatistics{};

                    //!TODO: reverse strand
                    stellarThreadTime.forward_strand_stellar_time.measure_time([&]()
                    {
                        // container for eps-matches
                        seqan2::StringSet<stellar::QueryMatches<stellar::StellarMatch<seqan2::String<TAlphabet> const,
                                                                                      seqan2::CharString> > > forwardMatches;
                        seqan2::resize(forwardMatches, length(queries));

                        constexpr bool databaseStrand = true;
                        stellar::QueryIDMap<TAlphabet> queryIDMap{queries};

                        stellar::StellarComputeStatistics statistics = stellar::StellarLauncher<TAlphabet>::search_and_verify
                        (
                            databaseSegment,
                            databaseID,
                            queryIDMap,
                            databaseStrand,
                            threadOptions,
                            swiftPattern,
                            stellarThreadTime.forward_strand_stellar_time.prefiltered_stellar_time,
                            forwardMatches
                        );

                        ld.text_out << std::endl; // swift filter output is on same line
                        stellar::_printDatabaseIdAndStellarKernelStatistics(threadOptions.verbose, databaseStrand, databaseID, statistics, ld.text_out);

                        stellarThreadTime.forward_strand_stellar_time.post_process_eps_matches_time.measure_time([&]()
                        {
                            // forwardMatches is an in-out parameter
                            // this is the match consolidation
                            stellar::_postproccessQueryMatches(databaseStrand, threadOptions.referenceLength, threadOptions,
                                                                    forwardMatches, disabledQueryIDs);
                        }); // measure_time

                        if (stellar::_shouldWriteOutputFile(databaseStrand, forwardMatches))
                        {
                            // open output files
                            std::ofstream outputFile(threadOptions.outputFile.c_str(), ::std::ios_base::out);
                            if (!outputFile.is_open())
                            {
                                std::cerr << "Could not open output file." << std::endl;
                                error_triggered = true;
                            }
                            stellarThreadTime.forward_strand_stellar_time.output_eps_matches_time.measure_time([&]()
                            {
                                // output forwardMatches on positive database strand
                                stellar::_writeAllQueryMatchesToFile(forwardMatches, queryIDs, databaseStrand, "gff", outputFile);
                            }); // measure_time
                        }

                        outputStatistics = stellar::_computeOutputStatistics(forwardMatches);
                    }); // measure_time

                    stellar::_writeOutputStatistics(outputStatistics, threadOptions.verbose, false /* disabledQueriesFile.is_open() */, ld.text_out);

                    ld.timeStatistics.emplace_back(stellarThreadTime.milliseconds());
                    if (arguments.write_time)
                    {
                        std::filesystem::path time_path =  cart_queries_path.string() + std::string(".gff.time");

                        stellar::_print_stellar_app_time(stellarThreadTime, ld.text_out);
                    }
                }
                else
                {
                    std::vector<std::string> process_args{};
                    process_args.insert(process_args.end(), {var_pack.stellar_exec, "--version-check", "0"});

                    if (segments && ref_meta)
                    {
                        // search segments of a single reference file
                        auto ref_len = ref_meta->total_len;
                        auto seg = segments->segment_from_bin(bin_id);
                        process_args.insert(process_args.end(), {index.bin_path()[0][0], std::string(cart_queries_path),
                                                                "--referenceLength", std::to_string(ref_len),
                                                                "--sequenceOfInterest", std::to_string(seg.ref_ind),
                                                                "--segmentBegin", std::to_string(seg.start),
                                                                "--segmentEnd", std::to_string(seg.start + seg.len)});
                    }
                    else
                    {
                        // search a reference database of bin sequence files
                        if (index.bin_path().size() < (size_t) bin_id) {
                            throw std::runtime_error("Could not find reference file with index " + std::to_string(bin_id) +
                            ". Did you forget to provide metadata to search segments in a single reference file instead?");
                        }
                        process_args.insert(process_args.end(), {index.bin_path()[bin_id][0], std::string(cart_queries_path)});
                    }

                    if (arguments.write_time)
                        process_args.insert(process_args.end(), "--time");

                    process_args.insert(process_args.end(), {"-e", std::to_string(er_rate),
                                                            "-l", std::to_string(arguments.pattern_size),
                                                            "-o", std::string(cart_queries_path) + ".gff"});

                    auto start = std::chrono::high_resolution_clock::now();
                    external_process process(process_args);
                    auto end = std::chrono::high_resolution_clock::now();

                    ld.timeStatistics.emplace_back(0.0 + std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count());

                    ld.text_out << process.cout();
                    ld.text_out << process.cerr();

                    if (process.status() != 0) {
                        std::unique_lock g(mutex); // make sure that our output is synchronized
                        std::cerr << "error running VALIK_STELLAR\n";
                        std::cerr << "call:";
                        for (auto args : process_args) {
                            std::cerr << " " << args;
                        }
                        std::cerr << '\n';
                        std::cerr << process.cerr() << '\n';
                        error_triggered = true;
                    }
                }
            }
        });
    }

    // prefilter data which inserts them into the shopping carts
    for (auto &&chunked_records : fin | seqan3::views::chunk((1ULL << 20) * 10))
    {
        query_records.clear();
        auto start = std::chrono::high_resolution_clock::now();
        for (auto && query_record: chunked_records)
            query_records.emplace_back(std::move(query_record.id()), std::move(query_record.sequence()));

        auto end = std::chrono::high_resolution_clock::now();
        time_statistics.reads_io_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

        start = std::chrono::high_resolution_clock::now();
        prefilter_queries_parallel(index.ibf(), arguments, query_records, thresholder, queue);
        end = std::chrono::high_resolution_clock::now();
        time_statistics.prefilter_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    }
    queue.finish(); // Flush carts that are not empty yet
    consumerThreads.clear();

    // Merge all local data together
    std::ofstream text_out(arguments.out_file.string() + ".out");
    std::vector<std::string> output_files;

    for (auto const& ld : localData) {
        output_files.insert(output_files.end(), ld.output_files.begin(), ld.output_files.end());
        time_statistics.cart_processing_times.insert(time_statistics.cart_processing_times.end(),
                                                     ld.timeStatistics.begin(), ld.timeStatistics.end());
        text_out << ld.text_out.str();
    }

    std::vector<std::string> merge_process_args{var_pack.merge_exec};
    for (auto & path : output_files)
        merge_process_args.push_back(path);
    external_process merge(merge_process_args);

    auto check_external_process_success = [&](std::vector<std::string> const & proc_args, external_process const & proc)
    {
        if (proc.status() != 0) {
            std::cerr << "External process failed\n";
            std::cerr << "call:";
            for (auto args : proc_args) {
                std::cerr << " " << args;
            }
            std::cerr << '\n';
            std::cerr << proc.cerr() << '\n';
            return true;
        }
        else
            return error_triggered;
    };

    error_triggered = check_external_process_success(merge_process_args, merge);

    std::ofstream matches_out(arguments.out_file);
    matches_out << merge.cout();
    return error_triggered;
}

void valik_search(search_arguments const & arguments)
{
    search_time_statistics time_statistics{};

    bool failed;
    if (arguments.compressed)
        failed = run_program<seqan3::data_layout::compressed>(arguments, time_statistics);
    else
        failed = run_program<seqan3::data_layout::uncompressed>(arguments, time_statistics);

    if (arguments.write_time)
        write_time_statistics(time_statistics, arguments.out_file.string() + ".time");

    if (failed) {
        throw std::runtime_error("valik_search failed. Run didn't complete correctly.");
    }
}

} // namespace valik::app
