#pragma once

#include <utilities/threshold/param_set.hpp>
#include <utilities/threshold/search_pattern.hpp>

#include <cereal/archives/binary.hpp> 
#include <cereal/types/vector.hpp>

#include <seqan3/core/debug_stream.hpp>

namespace valik
{

/**
 * @brief For a kmer size k consider how the FNR changes for various values of the parameters t, e and l. 
 *   
 * @param k Chosen k-mer size.
 * @param kmer_weight Less than k-mer size for gapped k-mers.
 * @param fn_conf_counts Number of configurations of e errors that do not retain at least t shared kmers after mutation.
*/
struct kmer_loss
{
    using row_t = std::vector<uint64_t>;
    using table_t = std::vector<row_t>;
    using mat_t = std::vector<table_t>;
    
    uint8_t k;
    uint8_t kmer_weight;
    seqan3::shape shape;
    mat_t fn_conf_counts;  // false negative configuration counts

    kmer_loss() noexcept = default;
    kmer_loss(kmer_loss const &) noexcept = default;
    kmer_loss & operator=(kmer_loss const &) noexcept = default;
    kmer_loss & operator=(kmer_loss &&) noexcept = default;
    ~kmer_loss() noexcept = default;

    /**
     * @brief For a maximum error count find the number of error configurations 
     *        that do not retain at least the threshold number of k-mers after mutation.
    */
    mat_t count_err_conf_below_thresh(param_space const & space)
    {
        mat_t matrix;
        for (uint8_t t = 1; t <= space.max_thresh; t++)
        {
            table_t table;
            for (uint8_t e = 0; e <= space.max_errors; e++)
            {
                row_t row;
                for(size_t l = 0; l <= space.max_len; l++)
                {       
                    int shared_base_count = l - e;
                    if ((int) (kmer_weight + t) - 1 > shared_base_count) // all error distributions destroy enough k-mers
                        row.push_back(combinations(e, l));
                    else if (e == 0)    // this has to be after the first condition; not everything in first row should be 0
                        row.push_back(0);
                    else
                    {                        
                        row.push_back(row.back() + table.back()[l - 1]);
                        row.back() -= table.back()[l - kmer_weight - 1];

                        for (uint8_t i = 1; i < t; i++)
                        {
                            row.back() -= matrix[matrix.size() - i][e - 1][l - kmer_weight - i - 1];
                            row.back() += matrix[matrix.size() - i][e - 1][l - kmer_weight - i];
                        }
                    }
                }
                table.push_back(row);
            }
            matrix.push_back(table);
        }
        return matrix;
    }

    kmer_loss(uint8_t const kmer_size, param_space const & space) : 
                    k(kmer_size),
                    kmer_weight(kmer_size),
                    fn_conf_counts(count_err_conf_below_thresh(space)) { }

    kmer_loss(seqan3::shape const kmer_shape, param_space const & space) : 
                    k(kmer_shape.size()), 
                    kmer_weight(kmer_shape.count()),
                    fn_conf_counts(count_err_conf_below_thresh(space)) { }

    /**
     * @brief False negative rate for a parameter set.
    */
    double fnr_for_param_set(search_pattern const & pattern, param_set const & params) const
    {
        //!TODO: differentiate between gapped and ungapped shapes
        if (kmer_lemma_threshold(pattern.l, params.kmer_weight, pattern.e) >= params.t)
            return 0.0;
        if (params.t > fn_conf_counts.size())
            throw std::runtime_error("Calculated configuration count table for threshold=[1, " + std::to_string(fn_conf_counts.size()) + "]. " 
                                     "Can't find FNR for threshold=" + std::to_string(params.t));
        if (pattern.e >= fn_conf_counts[0].size())
            throw std::runtime_error("Calculated configuration count table for errors=[0, " + std::to_string(fn_conf_counts[0].size()) + "]. " 
                                     "Can't find FNR for errors=" + std::to_string(pattern.e));
        if (pattern.l >= fn_conf_counts[0][0].size())
            throw std::runtime_error("Calculated configuration count table for min_len=[1, " + std::to_string(fn_conf_counts[0][0].size()) + "]. " 
                                     "Can't find FNR for min_len=" + std::to_string(pattern.l));
        return fn_conf_counts[params.t - 1][pattern.e][pattern.l] / (double) pattern.total_conf_count();
    }

    template<class Archive>
    void serialize(Archive & archive)
    {
      archive(k, kmer_weight, fn_conf_counts);
    }
};

}   // namespace valik
