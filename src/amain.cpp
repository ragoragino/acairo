#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <csignal>

#include <acairo.h>

volatile sig_atomic_t sig_flag = 0;

acairo::Task<void> handle_socket(std::shared_ptr<acairo::TCPStream> stream) {
    try {
        std::vector<char> vector_received_message = co_await stream->read(27);

        std::string received_message(vector_received_message.begin(), vector_received_message.end());
        
        std::cout << "Reading from socket was succesful:" << received_message << "\n"; 

        const std::string send_message = "Just nod if you can hear me!";
        std::vector<char> vector_message(send_message.begin(), send_message.end());
        co_await stream->write(std::move(vector_message));

        std::cout << "Writing to socket was successful." << "\n"; 
    } catch (const std::exception& e){
        std::cout << "handle_socket failed: " << e.what() << "\n"; 
    }
}

int main(){
    using namespace acairo;

    using namespace std::chrono_literals;

    SchedulerConfiguration scheduler_config{
        number_of_worker_threads: 10,
    };

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
        std::cout << "Unable to register signal handler.\n";
        exit(1);
    }

    auto shutdownHandler = std::thread([&](){
        while (sig_flag == 0) {
            std::this_thread::sleep_for(500ms);
        }

        std::cout << "Shutting down cairo.\n";

        listener.shutdown();

        executor->stop();
    });

    listener.bind("127.0.0.1:8080");

    while(true) {
        try { 
            auto stream = listener.accept();

            auto handler = std::bind(handle_socket, std::move(stream));

            executor->spawn(std::move(handler));
        } catch(const TCPListenerStoppedError& e) {
            if (sig_flag == 1) {
                break;
            }

            throw;
        }
    }

    shutdownHandler.join();

    return 0;
}