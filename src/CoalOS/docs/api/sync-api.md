# Synchronization API

This document describes the synchronization primitives available in Coal OS for managing concurrent access to shared resources.

## Overview

Coal OS provides several synchronization mechanisms:
- **Spinlocks**: Low-level locks for short critical sections
- **Mutexes**: Sleeping locks for longer operations (future)
- **Semaphores**: Resource counting primitives (future)
- **Wait Queues**: Condition variable mechanism
- **RW Locks**: Reader-writer locks (future)

## Spinlocks

Spinlocks are the primary synchronization primitive in the kernel. They are suitable for short critical sections where the lock holder does not sleep.

### Basic Spinlock Operations

```c
/**
 * @brief Initialize a spinlock
 * @param lock Spinlock to initialize
 */
void spinlock_init(spinlock_t *lock);

/**
 * @brief Acquire spinlock (busy wait)
 * @param lock Spinlock to acquire
 */
void spinlock_acquire(spinlock_t *lock);

/**
 * @brief Release spinlock
 * @param lock Spinlock to release
 */
void spinlock_release(spinlock_t *lock);

/**
 * @brief Try to acquire spinlock
 * @param lock Spinlock to try
 * @return true if acquired, false otherwise
 */
bool spinlock_try_acquire(spinlock_t *lock);
```

### IRQ-Safe Spinlocks

When spinlocks are used in interrupt context, interrupts must be disabled:

```c
/**
 * @brief Acquire spinlock and save interrupt state
 * @param lock Spinlock to acquire
 * @param flags Variable to store interrupt state
 */
void spinlock_acquire_irqsave(spinlock_t *lock, unsigned long *flags);

/**
 * @brief Release spinlock and restore interrupt state
 * @param lock Spinlock to release
 * @param flags Saved interrupt state
 */
void spinlock_release_irqrestore(spinlock_t *lock, unsigned long flags);

/**
 * @brief Acquire spinlock with interrupts disabled
 * @param lock Spinlock to acquire
 */
void spinlock_acquire_irq(spinlock_t *lock);

/**
 * @brief Release spinlock and enable interrupts
 * @param lock Spinlock to release
 */
void spinlock_release_irq(spinlock_t *lock);
```

### Spinlock Guidelines

```c
// Correct usage with interrupts
spinlock_t my_lock;
unsigned long flags;

spinlock_init(&my_lock);

// Critical section that may be interrupted
spinlock_acquire_irqsave(&my_lock, &flags);
// ... critical section ...
spinlock_release_irqrestore(&my_lock, flags);

// Critical section in interrupt handler
spinlock_acquire(&my_lock);  // Already in interrupt context
// ... critical section ...
spinlock_release(&my_lock);
```

## Wait Queues

Wait queues allow threads to sleep until a condition is met:

### Wait Queue Operations

```c
/**
 * @brief Initialize wait queue
 * @param queue Wait queue to initialize
 */
void wait_queue_init(wait_queue_t *queue);

/**
 * @brief Add current thread to wait queue
 * @param queue Wait queue
 * @param entry Wait queue entry for this thread
 */
void wait_queue_add(wait_queue_t *queue, wait_queue_entry_t *entry);

/**
 * @brief Remove thread from wait queue
 * @param queue Wait queue
 * @param entry Wait queue entry to remove
 */
void wait_queue_remove(wait_queue_t *queue, wait_queue_entry_t *entry);

/**
 * @brief Wait for condition
 * @param queue Wait queue
 * @param condition Function to check condition
 * @param arg Argument to condition function
 * @return 0 on success, negative error if interrupted
 */
int wait_event(wait_queue_t *queue, 
               bool (*condition)(void *), void *arg);

/**
 * @brief Wait for condition with timeout
 * @param queue Wait queue
 * @param condition Function to check condition
 * @param arg Argument to condition function
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int wait_event_timeout(wait_queue_t *queue,
                      bool (*condition)(void *), void *arg,
                      uint32_t timeout_ms);

/**
 * @brief Wait for condition (interruptible)
 * @param queue Wait queue
 * @param condition Function to check condition
 * @param arg Argument to condition function
 * @return 0 on success, -EINTR if interrupted
 */
int wait_event_interruptible(wait_queue_t *queue,
                            bool (*condition)(void *), void *arg);
```

### Wake Operations

```c
/**
 * @brief Wake one thread waiting on queue
 * @param queue Wait queue
 */
void wake_up(wait_queue_t *queue);

/**
 * @brief Wake all threads waiting on queue
 * @param queue Wait queue
 */
void wake_up_all(wait_queue_t *queue);

/**
 * @brief Wake N threads waiting on queue
 * @param queue Wait queue
 * @param nr Number of threads to wake
 */
void wake_up_nr(wait_queue_t *queue, int nr);
```

## Mutexes (Future)

Mutexes are sleeping locks suitable for longer critical sections:

```c
/**
 * @brief Initialize mutex
 * @param mutex Mutex to initialize
 */
void mutex_init(mutex_t *mutex);

/**
 * @brief Lock mutex (may sleep)
 * @param mutex Mutex to lock
 */
void mutex_lock(mutex_t *mutex);

/**
 * @brief Unlock mutex
 * @param mutex Mutex to unlock
 */
void mutex_unlock(mutex_t *mutex);

/**
 * @brief Try to lock mutex
 * @param mutex Mutex to try
 * @return true if locked, false otherwise
 */
bool mutex_trylock(mutex_t *mutex);

/**
 * @brief Lock mutex (interruptible)
 * @param mutex Mutex to lock
 * @return 0 on success, -EINTR if interrupted
 */
int mutex_lock_interruptible(mutex_t *mutex);
```

## Semaphores (Future)

Semaphores provide counting synchronization:

```c
/**
 * @brief Initialize semaphore
 * @param sem Semaphore to initialize
 * @param count Initial count
 */
void semaphore_init(semaphore_t *sem, int count);

/**
 * @brief Acquire semaphore (may sleep)
 * @param sem Semaphore to acquire
 */
void down(semaphore_t *sem);

/**
 * @brief Release semaphore
 * @param sem Semaphore to release
 */
void up(semaphore_t *sem);

/**
 * @brief Try to acquire semaphore
 * @param sem Semaphore to try
 * @return true if acquired, false otherwise
 */
bool down_trylock(semaphore_t *sem);

/**
 * @brief Acquire semaphore with timeout
 * @param sem Semaphore to acquire
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int down_timeout(semaphore_t *sem, uint32_t timeout_ms);
```

## Reader-Writer Locks (Future)

RW locks allow multiple readers or a single writer:

```c
/**
 * @brief Initialize RW lock
 * @param lock RW lock to initialize
 */
void rwlock_init(rwlock_t *lock);

/**
 * @brief Acquire read lock
 * @param lock RW lock
 */
void read_lock(rwlock_t *lock);

/**
 * @brief Release read lock
 * @param lock RW lock
 */
void read_unlock(rwlock_t *lock);

/**
 * @brief Acquire write lock
 * @param lock RW lock
 */
void write_lock(rwlock_t *lock);

/**
 * @brief Release write lock
 * @param lock RW lock
 */
void write_unlock(rwlock_t *lock);

/**
 * @brief Acquire read lock with IRQ save
 * @param lock RW lock
 * @param flags Interrupt state storage
 */
void read_lock_irqsave(rwlock_t *lock, unsigned long *flags);

/**
 * @brief Acquire write lock with IRQ save
 * @param lock RW lock
 * @param flags Interrupt state storage
 */
void write_lock_irqsave(rwlock_t *lock, unsigned long *flags);
```

## Atomic Operations

For lock-free programming:

```c
/**
 * @brief Atomic read
 * @param v Atomic variable
 * @return Current value
 */
int atomic_read(atomic_t *v);

/**
 * @brief Atomic set
 * @param v Atomic variable
 * @param i New value
 */
void atomic_set(atomic_t *v, int i);

/**
 * @brief Atomic add
 * @param i Value to add
 * @param v Atomic variable
 */
void atomic_add(int i, atomic_t *v);

/**
 * @brief Atomic subtract
 * @param i Value to subtract
 * @param v Atomic variable
 */
void atomic_sub(int i, atomic_t *v);

/**
 * @brief Atomic increment
 * @param v Atomic variable
 */
void atomic_inc(atomic_t *v);

/**
 * @brief Atomic decrement
 * @param v Atomic variable
 */
void atomic_dec(atomic_t *v);

/**
 * @brief Atomic compare and swap
 * @param v Atomic variable
 * @param old Expected old value
 * @param new New value
 * @return true if swapped, false otherwise
 */
bool atomic_cmpxchg(atomic_t *v, int old, int new);

/**
 * @brief Atomic test and set bit
 * @param nr Bit number
 * @param addr Address
 * @return Previous bit value
 */
bool test_and_set_bit(int nr, volatile unsigned long *addr);

/**
 * @brief Atomic test and clear bit
 * @param nr Bit number
 * @param addr Address
 * @return Previous bit value
 */
bool test_and_clear_bit(int nr, volatile unsigned long *addr);
```

## Memory Barriers

Ensure memory ordering:

```c
/**
 * @brief Full memory barrier
 */
void mb(void);

/**
 * @brief Read memory barrier
 */
void rmb(void);

/**
 * @brief Write memory barrier
 */
void wmb(void);

/**
 * @brief Compiler barrier
 */
#define barrier() __asm__ __volatile__("": : :"memory")
```

## Usage Examples

### Producer-Consumer with Wait Queue

```c
// Shared data
struct {
    spinlock_t lock;
    wait_queue_t queue;
    bool data_ready;
    int data;
} shared;

// Initialize
spinlock_init(&shared.lock);
wait_queue_init(&shared.queue);
shared.data_ready = false;

// Consumer thread
bool check_data_ready(void *arg) {
    return shared.data_ready;
}

void consumer(void) {
    while (1) {
        // Wait for data
        wait_event(&shared.queue, check_data_ready, NULL);
        
        // Process data
        unsigned long flags;
        spinlock_acquire_irqsave(&shared.lock, &flags);
        int value = shared.data;
        shared.data_ready = false;
        spinlock_release_irqrestore(&shared.lock, flags);
        
        kprintf("Consumed: %d\n", value);
    }
}

// Producer thread
void producer(void) {
    int value = 0;
    while (1) {
        // Produce data
        sleep(1000);  // Simulate work
        
        // Store data
        unsigned long flags;
        spinlock_acquire_irqsave(&shared.lock, &flags);
        shared.data = value++;
        shared.data_ready = true;
        spinlock_release_irqrestore(&shared.lock, flags);
        
        // Wake consumer
        wake_up(&shared.queue);
    }
}
```

### Reference Counting

```c
typedef struct {
    atomic_t refcount;
    spinlock_t lock;
    // ... other fields ...
} object_t;

// Initialize object
object_t* object_create(void) {
    object_t *obj = kmalloc(sizeof(object_t));
    if (obj) {
        atomic_set(&obj->refcount, 1);
        spinlock_init(&obj->lock);
    }
    return obj;
}

// Get reference
void object_get(object_t *obj) {
    atomic_inc(&obj->refcount);
}

// Put reference
void object_put(object_t *obj) {
    if (atomic_dec_and_test(&obj->refcount)) {
        // Last reference, free object
        kfree(obj);
    }
}
```

### Reader-Writer Pattern (with spinlocks)

```c
typedef struct {
    spinlock_t lock;
    int readers;
    bool writer;
    wait_queue_t read_queue;
    wait_queue_t write_queue;
} rwlock_emulation_t;

// Acquire for reading
void acquire_read(rwlock_emulation_t *rw) {
    unsigned long flags;
    spinlock_acquire_irqsave(&rw->lock, &flags);
    
    while (rw->writer) {
        spinlock_release_irqrestore(&rw->lock, flags);
        wait_event(&rw->read_queue, !rw->writer, NULL);
        spinlock_acquire_irqsave(&rw->lock, &flags);
    }
    
    rw->readers++;
    spinlock_release_irqrestore(&rw->lock, flags);
}

// Acquire for writing
void acquire_write(rwlock_emulation_t *rw) {
    unsigned long flags;
    spinlock_acquire_irqsave(&rw->lock, &flags);
    
    while (rw->writer || rw->readers > 0) {
        spinlock_release_irqrestore(&rw->lock, flags);
        wait_event(&rw->write_queue, 
                  !rw->writer && rw->readers == 0, NULL);
        spinlock_acquire_irqsave(&rw->lock, &flags);
    }
    
    rw->writer = true;
    spinlock_release_irqrestore(&rw->lock, flags);
}
```

## Deadlock Prevention

Guidelines to prevent deadlocks:

1. **Lock Ordering**: Always acquire locks in the same order
2. **No Nested Spinlocks**: Avoid holding multiple spinlocks
3. **No Sleeping**: Never sleep while holding a spinlock
4. **Timeout**: Use timeouts for blocking operations
5. **Lock-Free**: Use atomic operations when possible

## Performance Considerations

1. **Minimize Lock Time**: Keep critical sections short
2. **Use Appropriate Primitive**: 
   - Spinlock for short sections
   - Mutex for long sections
   - RW lock for read-heavy workloads
3. **Avoid Contention**: Design to minimize lock conflicts
4. **Cache Alignment**: Align locks to cache lines
5. **Lock-Free Algorithms**: Consider for hot paths

## Debugging Support

```c
/**
 * @brief Enable lock debugging
 */
void lock_debug_enable(void);

/**
 * @brief Check for lock order violations
 */
void lock_debug_check_order(void);

/**
 * @brief Print lock statistics
 */
void lock_debug_print_stats(void);

/**
 * @brief Detect potential deadlocks
 */
void lock_debug_detect_deadlock(void);
```