#### State
Still work in progress!

#### Best materials
- https://blog.panicsoftware.com/co_awaiting-coroutines/
- https://lewissbaker.github.io/2017/11/17/understanding-operator-co-await
- https://www.dwcomputersolutions.net/posts/coroutines5/
- https://en.cppreference.com/w/cpp/language/coroutines
- https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer


// Valgrind
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=valgrind.txt ./acairomain

```
{
  promise_type promise;
  co_await promise.initial_suspend();
  try { F; }
  catch (...) { promise.unhandled_exception(); }
final_suspend:
  co_await promise.final_suspend();
}
```