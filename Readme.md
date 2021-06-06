# Handle thread exceptions
# Proper logging library
# Fix handling of epoll_wait


1. Compiler creates promise object on the stack of the coroutine
2. promise.get_return_object() returns the resumable objects (return object of the coroutine)

resumable -> created from promise_type (resumable defines promise_type) -> coroutine_handle (holds coroutine state) takes promise_type as template