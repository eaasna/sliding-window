#include <valik/search/search.hpp>

namespace valik::app
{

/**
 * @brief Function that loads the index and launches local or distributed search.
 *
 * @tparam compressed Interleaved Bloom Filter layout type.
 * @param arguments Command line arguments.
 */
void valik_search(search_arguments const & arguments)
{

    search_time_statistics time_statistics{};

    bool failed;
    if (arguments.distribute)
    {
        if (arguments.compressed)
        {
            if (arguments.manual_threshold)
                failed = search_distributed<true, true>(arguments, time_statistics);
            else
                failed = search_distributed<true, false>(arguments, time_statistics);
        }
        else
        {
            if (arguments.manual_threshold)
                failed = search_distributed<false, true>(arguments, time_statistics);
            else
                failed = search_distributed<false, false>(arguments, time_statistics);
        }
    }

    // Shared memory execution
    else
    {
        if (arguments.compressed)
        {
            if (arguments.query_meta_path.empty())
            {
                if (arguments.manual_threshold)
                    failed = search_local<true, false, true>(arguments, time_statistics);
                else
                    failed = search_local<true, false, false>(arguments, time_statistics);
            }
            else // Split long query sequences
            {
                if (arguments.manual_threshold)
                    failed = search_local<true, true, true>(arguments, time_statistics);
                else
                    failed = search_local<true, true, false>(arguments, time_statistics);
            }
        }
        else
        {
            if (arguments.query_meta_path.empty())
            {
                if (arguments.manual_threshold)
                    failed = search_local<false, false, true>(arguments, time_statistics);
                else
                    failed = search_local<false, false, false>(arguments, time_statistics);
            }
            else // Split long query sequences
            {
                if (arguments.manual_threshold)
                    failed = search_local<false, true, true>(arguments, time_statistics);
                else
                    failed = search_local<false, true, false>(arguments, time_statistics);
            }
        }
    }

    // Consolidate matches (not necessary when searching a metagenomic database)
    auto start = std::chrono::high_resolution_clock::now();
    if (!arguments.ref_meta_path.empty())
    {
        consolidate_matches(arguments);
        const bool error_in_delete = !std::filesystem::remove(arguments.all_matches);
        if (error_in_delete)
            std::cerr << "Could not clean up intermediate file: \t" << std::string(arguments.all_matches) << '\n';
        failed = failed || error_in_delete;
    }
    auto end = std::chrono::high_resolution_clock::now();
    time_statistics.consolidation_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

    if (arguments.write_time)
        write_time_statistics(time_statistics, arguments.out_file.string() + ".time", arguments);

    if (failed)
        throw std::runtime_error("valik_search failed. Run didn't complete correctly.");
}

} // namespace valik::app
