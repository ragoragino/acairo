package main

import (
	"fmt"
	"io/ioutil"
	"net"
	"sync"
)

func main() {
	numberOfWorkers := 1000
	tcpAddressStr := "localhost:8080"
	message := "Is there anybody out there?"
	errChannel := make(chan error, numberOfWorkers)

	wg := sync.WaitGroup{}
	wg.Add(numberOfWorkers)

	tcpAddr, err := net.ResolveTCPAddr("tcp4", tcpAddressStr)
	if err != nil {
		panic(err)
	}

	for i := 0; i != numberOfWorkers; i++ {
		go func(id int) {
			defer wg.Done()

			conn, err := net.DialTCP("tcp", nil, tcpAddr)
			if err != nil {
				errChannel <- fmt.Errorf("Worker [%d]: Error: %v", id, err)
				return
			}

			defer func() {
				err = conn.Close()
				if err != nil {
					fmt.Printf("Worker [%d]: Error: %v", id, err)
				}
			}()

			fmt.Printf("Worker [%d]: Writing.\n", id)

			_, err = conn.Write([]byte(message))
			if err != nil {
				errChannel <- fmt.Errorf("Worker [%d]: Error: %v", id, err)
				return
			}

			fmt.Printf("Worker [%d]: Reading\n", id)

			result, err := ioutil.ReadAll(conn)
			if err != nil {
				errChannel <- fmt.Errorf("Worker [%d]: Error: %v", id, err)
				return
			}

			fmt.Printf("Worker [%d]: Received: %s.\n", id, result)

			errChannel <- nil
		}(i)
	}

	wg.Wait()

	close(errChannel)

	for err := range errChannel {
		if err != nil {
			panic(err)
		}
	}
}
