#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <cstring>
#include <csignal>
#include <semaphore.h>

#include "acairo.h"
#include "logger.hpp"

sem_t shutdown_semaphore;

acairo::Task<void> handle_socket(std::shared_ptr<acairo::TCPStream> stream) {
    auto l = logger::Logger();

    std::vector<char> vector_received_message = co_await stream->read(27);

    const std::string received_message(vector_received_message.begin(), vector_received_message.end());
    
    LOG(l, logger::debug) << "Reading from socket was succesful:" << received_message; 

    const std::string send_message = "Just nod if you can hear me!";
    std::vector<char> vector_message(send_message.begin(), send_message.end());
    co_await stream->write(std::move(vector_message));

    LOG(l, logger::debug) << "Writing to socket was successful.";
}

acairo::Task<void> handle_accept(std::shared_ptr<acairo::Executor> executor,
    const acairo::TCPListener& listener) {   
    while (true) {
        auto stream = co_await listener.accept();

        auto handler = std::bind(handle_socket, stream);

        executor->spawn(std::move(handler));
    }
}

// Replicating the behaviour of Golang's defer, where
// funcs are run before exiting function scope.
// Here we use it to invoke destructors for non-RAII objects.
using defer = std::shared_ptr<void>;

int main(){
    using namespace acairo;

    using namespace std::chrono_literals;

    // Initialize logger
    auto log_config = logger::Configuration("debug");
    logger::InitializeGlobalLogger(log_config);
    auto l = logger::Logger().WithPair("Component", "main");

    // Initialize acairo components
    SchedulerConfiguration scheduler_config{
        number_of_worker_threads: 1,
    };

    ExecutorConfiguration executor_config{
       scheduler_config: scheduler_config,
       max_number_of_fds: 1024,
    };

    TCPStreamConfiguration tcpstream_configuration{};

    TCPListenerConfiguration tcplistener_config{
        stream_config: tcpstream_configuration,
        max_number_of_queued_conns: 1024,
    };

    auto executor = std::make_shared<Executor>(executor_config);
    TCPListener listener(tcplistener_config, executor);

    // Initialize semaphore and also its destructor
    sem_init(&shutdown_semaphore, 0, 0);
    defer _(nullptr, [l](...){
        if (int error_code = sem_destroy(&shutdown_semaphore); error_code < 0) {
            std::stringstream ss{};
            ss << "Unable to destroy semaphore: " << strerror(errno) << ".";
            LOG(l, logger::error) << ss.str(); 
        };
    });

    // https://www.informit.com/articles/article.aspx?p=2204014
    if (std::signal(SIGINT, [](int) -> void {
        // sem_post() is async-signal-safe: it may be safely called within a
        // signal handler: https://man7.org/linux/man-pages/man3/sem_post.3.html
        sem_post(&shutdown_semaphore); 
    }) == SIG_ERR) {
        LOG(l, logger::error) << "Unable to register signal handler.";
        exit(1);
    }

    // Create a handler that will watch for shutdown signals
    // and will stop all the components when it will receive one
    auto shutdown_handler = std::thread([&](){
        sem_wait(&shutdown_semaphore); 
        
        auto  l = logger::Logger();

        LOG(l, logger::debug) << "Shutting down cairo.";

        listener.stop();

        LOG(l, logger::debug) << "Listener stopped.";

        executor->stop();

        LOG(l, logger::debug) << "Executor stopped.";
    });

    listener.bind("127.0.0.1:8080");
    listener.listen();

    LOG(l, logger::info) << "Starting to accept new connections.";

    try { 
        auto f = std::bind(handle_accept, executor, std::ref(listener));
        executor->sync_wait(std::move(f));
    } catch (const TCPListenerStoppedError& e) {
        // This is an exception that can occur during the stopping of Executor,
        // so we can safely ignore it.
        LOG(l, logger::warn) << "sync_wait stopped with: " << e.what(); 
    } catch (const std::exception& e) {
        LOG(l, logger::info) << "sync_wait failed with: " << e.what(); 

        sem_post(&shutdown_semaphore); 
        shutdown_handler.join();

        throw;
    }
    
    shutdown_handler.join();

    return 0;
}