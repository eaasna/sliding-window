#!/bin/bash
cd consolidate
set -Eeuo pipefail

rm -f *_full.gff
rm -f *bins*overlap_segment_metadata.tsv
rm -f *bins*overlap_reference_metadata.tsv

ref_file=multi_seq_ref.fasta
read_length=150
read_dir=reads_$read_length
mkdir -p $read_dir
query_file=multi_seq_query.fasta
read_count=2
error_rate=0.06

i=1
# make multiple copies of identical chromosomes so that each read has multiple matches across the genome
for length in 560 560 560 464 464 464 464 210 
do
    echo "Simulating chromosome with length $length"
    chr_out="chr"$i".fasta"
    if (( $length == 560 )) ; then
        SEED=102
    elif (( $length == 464 )) ; then
        SEED=19
    else
        SEED=42
    fi
    mason_genome -l $length -o $chr_out -s $SEED &>/dev/null

    sed -i "s/^>.*$/>chr$i/g" $chr_out
    
    #----------- Sample reads from reference sequence -----------
    echo "Generating $read_count reads of length $read_length with error rate $error_rate"
    generate_local_matches \
        --output $read_dir \
        --max-error-rate $error_rate \
        --num-matches $read_count \
        --min-match-length $read_length \
        --max-match-length $read_length \
        --reverse \
        --ref-len $length \
        --seed $SEED \
        $chr_out
    
    sed -i "s/@.*/&_$i/" $read_dir/chr$i.fastq
    let i=i+1
done

# duplicate the sequence of the last chromosome so that reads have multiple matches per chromosome
cat chr$(expr $i - 1).fasta > chr${i}.fasta
tail -n +2 chr$(expr $i - 1).fasta >> chr${i}.fasta
tail -n +2 chr$(expr $i - 1).fasta >> chr${i}.fasta
rm chr$(expr $i - 1).fasta

cat chr*.fasta > $ref_file
rm chr*.fasta
cat $read_dir/chr*.fastq > query_tmp.fastq
rm -r $read_dir
sed -n '1~4s/^@/>/p;2~4p' query_tmp.fastq > $query_file
rm query_tmp.fastq

min_len=50

for bin in 8 16
do
        valik split $ref_file --out ${bin}bins${min_len}overlap_reference_metadata.tsv --seg-count $bin --overlap $min_len

        tail -n $((bin + 1)) ${bin}bins${min_len}overlap_reference_metadata.tsv | head -n $bin > segments.tsv
        while read -r bin_id id start len;
        do
                end=$(echo $start + $len | bc)
                stellar -e $error_rate -l $min_len -o multi_seq_ref_${id}_${start}_${len}.gff \
                        --sequenceOfInterest $id --segmentBegin $start \
                        --segmentEnd $end $ref_file $query_file > /dev/null

        done < segments.tsv

        rm segments.tsv


        cat multi_seq_ref_*.gff > ${bin}bins${min_len}overlap_dream_all.gff
        rm multi_seq_ref_*
done
