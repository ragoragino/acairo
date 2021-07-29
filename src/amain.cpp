#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <csignal>

#include "acairo.h"
#include "logger.hpp"

volatile sig_atomic_t sig_flag = 0;
volatile sig_atomic_t force_shutdown = 0;

acairo::Task<void> handle_socket(std::shared_ptr<acairo::TCPStream> stream) {
    auto l = logger::Logger();

    try {
        std::vector<char> vector_received_message = co_await stream->read(27);

        const std::string received_message(vector_received_message.begin(), vector_received_message.end());
        
        LOG(l, logger::debug) << "Reading from socket was succesful:" << received_message; 

        const std::string send_message = "Just nod if you can hear me!";
        std::vector<char> vector_message(send_message.begin(), send_message.end());
        co_await stream->write(std::move(vector_message));

        LOG(l, logger::debug) << "Writing to socket was successful."; 
    } catch (const std::exception& e){
        LOG(l, logger::error) << "handle_socket failed: " << e.what(); 
    }
}

int main(){
    using namespace acairo;

    using namespace std::chrono_literals;

    // Initialize logger
    auto log_config = logger::Configuration("debug");
    logger::InitializeGlobalLogger(log_config);
    auto l = logger::Logger().WithPair("Component", "main");

    SchedulerConfiguration scheduler_config{
        number_of_worker_threads: 10,
    };

    // Initialize acairo components
    ExecutorConfiguration executor_config{
       scheduler_config: scheduler_config,
       max_number_of_fds: 1024,
    };

    TCPStreamConfiguration tcpstream_configuration{
        read_timeout: 5s,
        write_timeout: 5s,
    };

    TCPListenerConfiguration tcplistener_config{
        stream_config: tcpstream_configuration,
        max_number_of_fds: 1024,
        max_number_of_queued_conns: 1024,
    };

    auto executor = std::make_shared<Executor>(executor_config);
    TCPListener listener(tcplistener_config, executor);

    // https://www.informit.com/articles/article.aspx?p=2204014
    if (std::signal(SIGINT, [](int) -> void {
        sig_flag = 1;
    }) == SIG_ERR) {
        LOG(l, logger::error) << "Unable to register signal handler.";
        exit(1);
    }

    // Create a handler that will watch shutdown signal and custom flags 
    // and will stop acairo's listener and executor
    auto shutdown_handler = std::thread([&](){
        while (sig_flag == 0 && force_shutdown == 0) {
            std::this_thread::sleep_for(500ms);
        }

        LOG(l, logger::debug) << "Shutting down cairo.";

        listener.shutdown();

        LOG(l, logger::debug) << "Listener stopped.";

        executor->stop();

        LOG(l, logger::debug) << "Executor stopped.";
    });

    listener.bind("127.0.0.1:8080");

    LOG(l, logger::info) << "Starting to accept new connections.";

    while(true) {
        try { 
            auto stream = listener.accept();

            auto handler = std::bind(handle_socket, stream);

            executor->spawn(std::move(handler));
        } catch(const TCPListenerStoppedError& e) {
            if (sig_flag == 1) {
                break;
            }

            force_shutdown = 1;
            shutdown_handler.join();

            throw;
        } catch(const std::exception& e) {
            force_shutdown = 1;
            shutdown_handler.join();

            throw;
        }
    }

    shutdown_handler.join();

    return 0;
}