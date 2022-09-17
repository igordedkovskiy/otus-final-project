#include <vector>
#include <list>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <numeric>
#include <chrono>
#include "gtest/gtest.h"

#include "Queue.hpp"
#include "serialization.hpp"
#include "ProducerConsumer.hpp"


/// \brief Compare execution time in two cases. First is one producer, one consumer.
///        Second is multiple producers, multiple consumers.
TEST(TEST_QUEUE, producer_consumer)
{
    const auto num_of_cores {std::thread::hardware_concurrency()};
    std::cout << "number of cores is " << num_of_cores << std::endl;
    if(num_of_cores == 1)
        return;

    using namespace std::chrono;
    using namespace threadsafe_containers;
    using clock = system_clock;
    using data_t = std::uint64_t;
    using data_collection_t = std::vector<data_t>;
    using all_data_collection_t = std::vector<data_collection_t>;
    using num_of_elements_t = std::size_t;
    using ranges_t = std::vector<num_of_elements_t>;

    auto make_data = [](std::size_t num_of_producers, const ranges_t& ranges)
    {
        all_data_collection_t data;
        data.reserve(num_of_producers);
        for(auto num_of_elements:ranges)
        {
            data_collection_t element;
            element.reserve(num_of_elements);
            for(std::size_t cntr {0}; cntr < num_of_elements; ++cntr)
                element.emplace_back(cntr * 13);
            data.emplace_back(std::move(element));
        }
        return data;
    };

    auto run = [&make_data]
            (std::size_t num_of_producers, std::size_t num_of_consumers, std::size_t num_of_elements)
    {
        auto split = [&num_of_elements, &num_of_producers]()
        {
            if(num_of_producers == 1)
                return ranges_t(1, num_of_elements);
            const auto size {num_of_elements / num_of_producers};
            ranges_t ranges;
            ranges.reserve(num_of_producers);
            for(std::size_t cntr {0}; cntr < num_of_producers - 1; ++cntr)
                ranges.push_back(size);
            ranges.push_back(num_of_elements - size * (num_of_producers - 1));
            return ranges;
        };

        const auto ranges {split()};
        const auto data {make_data(num_of_producers, ranges)};

        using threads_cntr_t = std::atomic<std::size_t>;
        threads_cntr_t producers_left {num_of_producers};
        threads_cntr_t consumers_left {num_of_consumers};

        using queue_t = Queue<data_t>;

        std::size_t producers_cntr {0};
        std::mutex cntr_mutex;
        auto producer = [&data, &producers_cntr, &cntr_mutex, &producers_left]
                (queue_t& queue)
        {
            try
            {
                auto get = [&data, &producers_cntr, &cntr_mutex]()
                {
                    std::scoped_lock lk {cntr_mutex};
                    const auto& d {data[producers_cntr]};
                    ++producers_cntr;
                    return d;
                };

                const auto d {get()};
                for(auto el:d)
                    queue.wait_if_full_push(el);
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << std::endl;
            }
            --producers_left;
        };

        auto consumer = [&consumers_left](queue_t& queue)
        {
            try
            {
                while(!queue.empty())
                {
                    do
                    {
                        auto el {queue.pop()};
                        if(el)
                        {
                            std::vector<data_t> v;
                            constexpr std::size_t N {100000};
                            v.reserve(N);
                            for(std::size_t cntr {0}; cntr < N; ++cntr)
                                v.emplace_back(cntr + *el);
                        }
                    }
                    while(!queue.empty());
                    std::this_thread::sleep_for(milliseconds(10));
                }
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << std::endl;
            }
            --consumers_left;
        };

        queue_t queue;

        std::vector<std::jthread> producers;
        producers.reserve(num_of_producers);
        const auto start {clock::now()};
        for(std::size_t cntr {0}; cntr < num_of_producers; ++cntr)
            producers.emplace_back(producer, std::ref(queue));

        std::vector<std::jthread> consumers;
        consumers.reserve(num_of_consumers);
        for(std::size_t cntr {0}; cntr < num_of_consumers; ++cntr)
            consumers.emplace_back(consumer, std::ref(queue));

        for(auto& cons:consumers)
        {
            if(cons.joinable())
                cons.detach();
        }
        for(auto& prod:producers)
        {
            if(prod.joinable())
                prod.detach();
        }

        // main thread
        {
            using namespace std::chrono_literals;
            while(!queue.empty())
            {
                // sleep
                // save queue (serialize)
                // etc.
                std::this_thread::sleep_for(10ms);
                //auto t {system_clock::to_time_t(system_clock::now())};
                //auto now {*std::localtime(&t)};
                //std::string time
                //{
                //    std::to_string(now.tm_year + 1900) + '.' +
                //            std::to_string(now.tm_mon + 1) + '.' +
                //            std::to_string(now.tm_mday) + '-' +
                //            std::to_string(now.tm_hour) + '.' +
                //            std::to_string(now.tm_min) + '.' +
                //            std::to_string(now.tm_sec)
                //};
                //using namespace serialization;
                //Serializer<queue_t, ArchiveType::TEXT> s{"qarchive-" + time + ".txt"};
                //s << queue;
            }
            while(!queue.empty())
                std::this_thread::sleep_for(10ms);

            while(consumers_left || producers_left)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return std::make_pair(duration_cast<milliseconds>(clock::now() - start).count(), queue.empty());
    };

    try
    {
        constexpr std::size_t num_of_elements {1000};
        /// \note one producers / consumers
        const auto [single_duration, result] {run(1, 1, num_of_elements)};
        ASSERT_TRUE(result);

        for(std::size_t mfactor {1}; mfactor < 6; ++mfactor)
        {
            /// \note multiple producers / consumers. Number of consumers = number of producers.
            const std::size_t num_of_producers { static_cast<std::remove_cv_t<decltype(num_of_producers)>>
                (num_of_cores > 2 ? 0.5 * num_of_cores * mfactor: num_of_cores * mfactor)};
            const auto [multiple_duration, result2]{run(num_of_producers, num_of_producers, num_of_elements)};
            ASSERT_TRUE(result2);

            std::cout << "duration single|multiple (ms): "
                      << single_duration << '|' << multiple_duration
                      << "\tnumber of producers|consumers: "
                      << num_of_producers << '|' << num_of_producers
                      << std::endl;
            EXPECT_GE(single_duration, multiple_duration);
        }
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }
}


TEST(TEST_QUEUE, producer_consumer_framework)
{
    const auto num_of_cores {std::thread::hardware_concurrency()};
    std::cout << "number of cores is " << num_of_cores << std::endl;
    if(num_of_cores == 1)
        return;

    using namespace std::chrono;
    using namespace threadsafe_containers;
    using clock = system_clock;
    using data_t = std::uint64_t;
    using data_collection_t = std::vector<data_t>;
    using all_data_collection_t = std::vector<data_collection_t>;
    using cond_cntr_t = std::atomic_llong;
    using num_of_elements_t = std::size_t;
    using ranges_t = std::vector<num_of_elements_t>;
    using queue_t = Queue<data_t>;

    auto make_data = [](std::size_t num_of_producers, const ranges_t& ranges)
    {
        all_data_collection_t data;
        data.reserve(num_of_producers);
        for(auto num_of_elements:ranges)
        {
            data_collection_t element;
            element.reserve(num_of_elements);
            for(std::size_t cntr {0}; cntr < num_of_elements; ++cntr)
                element.emplace_back(cntr * 13);
            data.emplace_back(std::move(element));
        }
        return data;
    };

    auto run = [&make_data]
            (std::size_t num_of_producers, std::size_t num_of_consumers, std::size_t num_of_elements)
    {
        auto split = [&num_of_elements, &num_of_producers]()
        {
            if(num_of_producers == 1)
                return ranges_t(1, num_of_elements);
            const auto size {num_of_elements / num_of_producers};
            ranges_t ranges;
            ranges.reserve(num_of_producers);
            for(std::size_t cntr {0}; cntr < num_of_producers - 1; ++cntr)
                ranges.push_back(size);
            ranges.push_back(num_of_elements - size * (num_of_producers - 1));
            return ranges;
        };

        const auto ranges {split()};
        const auto data {make_data(num_of_producers, ranges)};
        cond_cntr_t elements_left (num_of_elements);

        using threads_cntr_t = std::atomic<std::size_t>;
        threads_cntr_t producers_cntr {0};
        std::mutex cntr_mutex;
        auto producer = [&data, &producers_cntr, &cntr_mutex]
                ([[maybe_unused]] std::stop_token stop_token, queue_t& queue)
        {
            try
            {
                cntr_mutex.lock();
                const auto& d {data[producers_cntr++]};
                cntr_mutex.unlock();
                for(auto el:d)
                    queue.wait_if_full_push(el);
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << std::endl;
            }
        };
        auto consumer = []([[maybe_unused]] std::stop_token stop_token, queue_t& queue)
        {
            try
            {
                while(!queue.empty())
                {
                    do
                    {
                        auto el {queue.pop()};
                        if(el)
                        {
                            std::vector<data_t> v;
                            constexpr std::size_t N {100000};
                            v.reserve(N);
                            for(std::size_t cntr {0}; cntr < N; ++cntr)
                                v.emplace_back(cntr + *el);
                        }
                    }
                    while(!queue.empty());
                    std::this_thread::sleep_for(milliseconds(10));
                }
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << std::endl;
            }
        };

        auto main_cycle = [](queue_t& queue)
        {
            try
            {
                using namespace std::chrono_literals;
                while(!queue.empty())
                {
                    // sleep
                    // save queue (serialize)
                    // etc.
                    std::this_thread::sleep_for(10ms);
                    //auto t {system_clock::to_time_t(system_clock::now())};
                    //auto now {*std::localtime(&t)};
                    //std::string time
                    //{
                    //    std::to_string(now.tm_year + 1900) + '.' +
                    //            std::to_string(now.tm_mon + 1) + '.' +
                    //            std::to_string(now.tm_mday) + '-' +
                    //            std::to_string(now.tm_hour) + '.' +
                    //            std::to_string(now.tm_min) + '.' +
                    //            std::to_string(now.tm_sec)
                    //};
                    //using namespace serialization;
                    //Serializer<queue_t, ArchiveType::TEXT> s{"qarchive-" + time + ".txt"};
                    //s << queue;
                }
                while(!queue.empty())
                    std::this_thread::sleep_for(10ms);
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << std::endl;
            }
        };

        using namespace producer_consumer;
        Framework<data_t> framework
        {
            producer, num_of_producers,
            consumer, num_of_consumers,
            main_cycle
        };

        bool queue_empty {true};
        const auto start {clock::now()};
        try
        {
            framework.run();
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << std::endl;
            queue_empty = false;
        }
        return std::make_pair(duration_cast<milliseconds>(clock::now() - start).count(), queue_empty);
    };

    try
    {
        constexpr std::size_t num_of_elements {1000};
        /// \note one producers / consumers
        const auto [single_duration, result] {run(1, 1, num_of_elements)};
        ASSERT_TRUE(result);

        for(std::size_t mfactor {1}; mfactor < 6; ++mfactor)
        {
            /// \note multiple producers / consumers. Number of consumers = number of producers.
            const std::size_t num_of_producers { static_cast<std::remove_cv_t<decltype(num_of_producers)>>
                (num_of_cores > 2 ? 0.5 * num_of_cores * mfactor: num_of_cores * mfactor)};
            const auto [multiple_duration, result2]{run(num_of_producers, num_of_producers, num_of_elements)};
            ASSERT_TRUE(result2);

            std::cout << "duration single|multiple (ms): "
                      << single_duration << '|' << multiple_duration
                      << "\tnumber of producers|consumers: "
                      << num_of_producers << '|' << num_of_producers
                      << std::endl;
            EXPECT_GE(single_duration, multiple_duration);
        }
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }
}

