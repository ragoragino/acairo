#### State
Still work in progress!

#### Best materials
- https://blog.panicsoftware.com/co_awaiting-coroutines/
- https://lewissbaker.github.io/2017/11/17/understanding-operator-co-await
- https://www.dwcomputersolutions.net/posts/coroutines5/
- https://en.cppreference.com/w/cpp/language/coroutines


// Valgrind
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=valgrind.txt ./acairomain

// TODO: Destroying chained coroutines after exceptions are thrown.