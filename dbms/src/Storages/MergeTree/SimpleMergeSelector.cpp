#include <Storages/MergeTree/SimpleMergeSelector.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Common/interpolate.h>

#include <cmath>
#include <iostream>
#include <regex>
#include <fstream>
#include <iterator>

#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
 
void mk_dir(std::string dir)
{
    DIR *mydir = NULL;
    if((mydir= opendir(dir.c_str()))==NULL)
        mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
    else
        closedir(mydir);
}

bool is_file_exist(std::string path)
{
    if (access(path.c_str(), F_OK) != -1)
    {
        return true;
    }
    return false;
}

int read_file_list(std::string path, std::vector<std::string> & result)
{
    DIR * dir;
    struct dirent * ptr;

    if ((dir = opendir(path.c_str())) == NULL)
    {
        return -1;
    }

    while ((ptr = readdir(dir)) != NULL)
    {
        if (strcmp(ptr->d_name, ".") == 0 || strcmp(ptr->d_name, "..") == 0) ///current dir OR parrent dir
        {
            continue;
        }
        else if (ptr->d_type == 8) ///file
        {
            result.push_back(std::string(ptr->d_name));
        }
    }

    closedir(dir);

    return 0;
}

int rm_file(std::string filename)
{
    if (remove(filename.c_str()) == 0)
        return 0;
    else
        return -1;
}

int string_split(const std::string & in, const std::string & delim, std::vector<std::string> & ret)
{
    try
    {
        std::regex re{delim};
        ret = std::vector<std::string>{std::sregex_token_iterator(in.begin(), in.end(), re, -1), std::sregex_token_iterator()};
    }
    catch (const std::exception & e)
    {
        return -1;
    }

    return 0;
}

int parse_file(const std::string& filename, std::vector<std::string>& part_list)
{
    std::ifstream in(filename);
    std::string line;

    if (in)
    {
        while (std::getline(in, line))
        {
            part_list.push_back(line);
        }

        return 0;
    }
    else
    {
        return -1;
    }
}

int read_base_from_file(int& base)
{
    std::ifstream in("/tmp/base");

    if (in)
    {
        std::istreambuf_iterator<char> begin(in);
        std::istreambuf_iterator<char> end;
        std::string some_str(begin, end);
        base = std::stoi(some_str);

        return 0;
    }
    else
    {
        return -1;
    }
}

namespace DB
{

namespace
{

/** Estimates best set of parts to merge within passed alternatives.
  */
struct Estimator
{
    using Iterator = SimpleMergeSelector::PartsInPartition::const_iterator;

    void consider(Iterator begin, Iterator end, size_t sum_size, size_t size_prev_at_left, const SimpleMergeSelector::Settings & settings)
    {
        double current_score = score(end - begin, sum_size, settings.size_fixed_cost_to_add);

        if (settings.enable_heuristic_to_align_parts
            && size_prev_at_left > sum_size * settings.heuristic_to_align_parts_min_ratio_of_sum_size_to_prev_part)
        {
            double difference = std::abs(log2(static_cast<double>(sum_size) / size_prev_at_left));
            if (difference < settings.heuristic_to_align_parts_max_absolute_difference_in_powers_of_two)
                current_score *= interpolateLinear(settings.heuristic_to_align_parts_max_score_adjustment, 1,
                    difference / settings.heuristic_to_align_parts_max_absolute_difference_in_powers_of_two);
        }

        if (settings.enable_heuristic_to_remove_small_parts_at_right)
            while (end >= begin + 3 && (end - 1)->size < settings.heuristic_to_remove_small_parts_at_right_max_ratio * sum_size)
                --end;

        if (!min_score || current_score < min_score)
        {
            min_score = current_score;
            best_begin = begin;
            best_end = end;
        }
    }

    SimpleMergeSelector::PartsInPartition getBest()
    {
        return SimpleMergeSelector::PartsInPartition(best_begin, best_end);
    }

    static double score(double count, double sum_size, double sum_size_fixed_cost)
    {
        /** Consider we have two alternative ranges of data parts to merge.
          * Assume time to merge a range is proportional to sum size of its parts.
          *
          * Cost of query execution is proportional to total number of data parts in a moment of time.
          * Let define our target: to minimize average (in time) total number of data parts.
          *
          * Let calculate integral of total number of parts, if we are going to do merge of one or another range.
          * It must be lower, and thus we decide, what range is better to merge.
          *
          * The integral is lower iff the following formula is lower:
          *
          *  sum_size / (count - 1)
          *
          * But we have some tunes to prefer longer ranges.
          */
        return (sum_size + sum_size_fixed_cost * count) / (count - 1.9);
    }

    double min_score = 0;
    Iterator best_begin {};
    Iterator best_end {};
};


/**
 * 1       _____
 *        /
 * 0_____/
 *      ^  ^
 *     min max
 */
double mapPiecewiseLinearToUnit(double value, double min, double max)
{
    return value <= min ? 0
        : (value >= max ? 1
        : ((value - min) / (max - min)));
}


/** Is allowed to merge parts in range with specific properties.
  */
bool allow(
    double sum_size,
    double max_size,
    double min_age,
    double range_size,
    double partition_size,
    const SimpleMergeSelector::Settings & settings,
    const int external_base)
{
//    std::cerr << "sum_size: " << sum_size << "\n";

    /// Map size to 0..1 using logarithmic scale
    double size_normalized = mapPiecewiseLinearToUnit(log1p(sum_size), log1p(settings.min_size_to_lower_base), log1p(settings.max_size_to_lower_base));

//    std::cerr << "size_normalized: " << size_normalized << "\n";

    /// Calculate boundaries for age
    double min_age_to_lower_base = interpolateLinear(settings.min_age_to_lower_base_at_min_size, settings.min_age_to_lower_base_at_max_size, size_normalized);
    double max_age_to_lower_base = interpolateLinear(settings.max_age_to_lower_base_at_min_size, settings.max_age_to_lower_base_at_max_size, size_normalized);

//    std::cerr << "min_age_to_lower_base: " << min_age_to_lower_base << "\n";
//    std::cerr << "max_age_to_lower_base: " << max_age_to_lower_base << "\n";

    /// Map age to 0..1
    double age_normalized = mapPiecewiseLinearToUnit(min_age, min_age_to_lower_base, max_age_to_lower_base);

//    std::cerr << "age: " << min_age << "\n";
//    std::cerr << "age_normalized: " << age_normalized << "\n";

    /// Map partition_size to 0..1
    double num_parts_normalized = mapPiecewiseLinearToUnit(partition_size, settings.min_parts_to_lower_base, settings.max_parts_to_lower_base);

//    std::cerr << "partition_size: " << partition_size << "\n";
//    std::cerr << "num_parts_normalized: " << num_parts_normalized << "\n";

    double combined_ratio = std::min(1.0, age_normalized + num_parts_normalized);

//    std::cerr << "combined_ratio: " << combined_ratio << "\n";

    double lowered_base = interpolateLinear(settings.base, 2.0, combined_ratio);

    if(external_base > 0) 
    {
        lowered_base = external_base;
    }

//    std::cerr << "------- lowered_base: " << lowered_base << "\n";

    return (sum_size + range_size * settings.size_fixed_cost_to_add) / (max_size + settings.size_fixed_cost_to_add) >= lowered_base;
}


void selectWithinPartition(
    const SimpleMergeSelector::PartsInPartition & parts,
    const size_t max_total_size_to_merge,
    Estimator & estimator,
    const SimpleMergeSelector::Settings & settings,
    const int external_base)
{
    size_t parts_count = parts.size();
    if (parts_count <= 1)
        return;

    for (size_t begin = 0; begin < parts_count; ++begin)
    {
        /// If too many parts, select only from first, to avoid complexity.
        if (begin > 1000)
            break;

        size_t sum_size = parts[begin].size;
        size_t max_size = parts[begin].size;
        size_t min_age = parts[begin].age;

        for (size_t end = begin + 2; end <= parts_count; ++end)
        {
            if (settings.max_parts_to_merge_at_once && end - begin > settings.max_parts_to_merge_at_once)
                break;

            size_t cur_size = parts[end - 1].size;
            size_t cur_age = parts[end - 1].age;

            sum_size += cur_size;
            max_size = std::max(max_size, cur_size);
            min_age = std::min(min_age, cur_age);

            if (max_total_size_to_merge && sum_size > max_total_size_to_merge)
                break;

            if (allow(sum_size, max_size, min_age, end - begin, parts_count, settings, external_base))
                estimator.consider(
                    parts.begin() + begin,
                    parts.begin() + end,
                    sum_size,
                    begin == 0 ? 0 : parts[begin - 1].size,
                    settings);
        }
    }
}

}

int SimpleMergeSelector::external_select(
    const Partitions & partitions,
    SimpleMergeSelector::PartsInPartition& merge_parts)
{
    std::string dir_name = "/tmp/merge/";
    mk_dir(dir_name);
    std::vector<std::string> file_list;
    int ret = read_file_list(dir_name, file_list);
    if(ret != 0) {
        LOG_ERROR(log, " read_file_list fail " << " ret " << ret);
        return -1;
    }
    if(file_list.size() < 1) {
        LOG_DEBUG(log, "empty dir");
        return -1;
    }

    for(const auto & path : file_list)
    {
        LOG_DEBUG(log, " file in dir " << path);
    }

    std::sort(file_list.begin(), file_list.end());

    std::vector<std::string> part_name_list;
    std::string path = dir_name + file_list[0];
    // TODO add flock
    ret = parse_file(path, part_name_list);
    if (ret != 0)
    {
        LOG_DEBUG(log, " parse fail " << " path " << path << " ret " << ret);
        return -1;
    }
    ret = rm_file(path);
    if( ret != 0 ) {
        LOG_ERROR(log, "rm_file fail" << path << " ret " << ret);
        return -1;
    }

    std::vector<std::string> cols;
    ret = string_split(part_name_list[0], "_", cols);
    if(ret != 0 || cols.size() < 1) {
        LOG_ERROR(log, " parse partition_id fail " << " part_name " << part_name_list[0]);
        return -1;
    }
    std::string filter_partition_id = cols[0];

    LOG_DEBUG(log, " external_select " << " file_list.size() " << file_list.size() 
        << " path " << path << " filter_partition_id " << filter_partition_id);

    for (const auto & partition : partitions)
    {
        std::string partition_id = "";
        for (const auto & part_info : partition)
        {
            auto part_ptr = *(const MergeTreeData::DataPartPtr *)(part_info.data);

            for (const auto & part_name : part_name_list)
            {
                if(partition_id.size() == 0) {
                    std::vector<std::string> tmp;
                    ret = string_split(part_ptr->name, "_", tmp);
                    if(ret != 0 || tmp.size() < 1) {
                        LOG_ERROR(log, " parse partition_id fail " << " part_name " << part_ptr->name);
                        return -1;
                    }
                    partition_id = tmp[0];

                    if( partition_id != filter_partition_id ) {
                        goto next_partition;
                    }
                }

                if (part_ptr->name == part_name)
                {
                    LOG_DEBUG(log, " found " << part_name);
                    merge_parts.push_back(part_info);
                    break;
                }
            }
        }
        next_partition:

        if (merge_parts.size() > 1)
        {
            // TODO: check range size
            // auto begin_pos = find(partition.begin(), partition.end(), merge_parts.front());
            // auto end_pos = find(partition.begin(), partition.end(), merge_parts.back());
            // int range_size = end_pos - begin_pos + 1;
            // if( range_size != merge_parts.size() ) {
                
            //     LOG_ERROR(log, " check range_size fail " << " range_size " << range_size 
            //         << " merge_parts.size() " << merge_parts.size());
            //     return -1;
            // }

            auto from_part_ptr = *(const MergeTreeData::DataPartPtr *)(merge_parts.front().data);
            auto to_part_ptr = *(const MergeTreeData::DataPartPtr *)(merge_parts.back().data);
            LOG_DEBUG(log, " merge found " << " from " << from_part_ptr->name << " to " << to_part_ptr->name);
            return 0;
        }
    }

    return -1;
}

SimpleMergeSelector::PartsInPartition SimpleMergeSelector::select(
    const Partitions & partitions,
    const size_t max_total_size_to_merge)
{
    Estimator estimator;

    SimpleMergeSelector::PartsInPartition merge_parts;
    int ret = external_select(partitions, merge_parts);
    if(ret == 0) 
    {
        return merge_parts;
    }
    else
    {
        LOG_DEBUG(log, "external_select fail" << " ret " << ret);
    }

    int external_base = 0;
    ret = read_base_from_file(external_base);
    LOG_DEBUG(log, " read_base_from_file " << " ret " << ret << " external_base " << external_base);

    for (const auto & partition : partitions)
        selectWithinPartition(partition, max_total_size_to_merge, estimator, settings, external_base);

    return estimator.getBest();
}

}
