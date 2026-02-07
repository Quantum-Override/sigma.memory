/*
 *  Test File: test_stack.c
 *  Description: Test NodeStack operations
 *  Date: 2026-02-07
 */

#include "internal/memory.h"
#include "internal/stack.h"
#include "memory.h"
// ----------------
#include <sigtest/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_stack.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "SYS0 should be ready during test setup");
}

static void set_teardown(void) {
    // Cleanup
}
#endif

#if 1  // Region: Stack Basic Operations Tests
void test_stack_initialized_empty(void) {
    Stack.clear();  // Ensure clean state
    Assert.isTrue(Stack.is_empty(), "Stack should be empty initially");
    Assert.isFalse(Stack.is_full(), "Stack should not be full initially");
    Assert.isTrue(Stack.depth() == 0, "Stack depth should be 0");
}

void test_stack_push_single_value(void) {
    Stack.clear();
    addr test_value = 0x1234ABCD;

    int result = Stack.push(test_value);
    Assert.isTrue(result == OK, "Push should succeed");
    Assert.isFalse(Stack.is_empty(), "Stack should not be empty after push");
    Assert.isTrue(Stack.depth() == 1, "Stack depth should be 1");
}

void test_stack_push_pop_value(void) {
    Stack.clear();
    addr test_value = 0xDEADBEEF;
    addr popped_value = 0;

    Stack.push(test_value);
    int result = Stack.pop(&popped_value);

    Assert.isTrue(result == OK, "Pop should succeed");
    Assert.isTrue(popped_value == test_value, "Popped value should match pushed value");
    Assert.isTrue(Stack.is_empty(), "Stack should be empty after pop");
    Assert.isTrue(Stack.depth() == 0, "Stack depth should be 0");
}

void test_stack_peek_preserves_value(void) {
    Stack.clear();
    addr test_value = 0xCAFEBABE;
    addr peeked_value = 0;

    Stack.push(test_value);
    int result = Stack.peek(&peeked_value);

    Assert.isTrue(result == OK, "Peek should succeed");
    Assert.isTrue(peeked_value == test_value, "Peeked value should match pushed value");
    Assert.isFalse(Stack.is_empty(), "Stack should not be empty after peek");
    Assert.isTrue(Stack.depth() == 1, "Stack depth should still be 1");
}

void test_stack_pop_empty_returns_error(void) {
    Stack.clear();
    addr value = 0;

    int result = Stack.pop(&value);
    Assert.isTrue(result == ERR, "Pop on empty stack should return ERR");
}

void test_stack_peek_empty_returns_error(void) {
    Stack.clear();
    addr value = 0;

    int result = Stack.peek(&value);
    Assert.isTrue(result == ERR, "Peek on empty stack should return ERR");
}

void test_stack_pop_with_null_returns_error(void) {
    Stack.clear();
    Stack.push(0x1234);

    int result = Stack.pop(NULL);
    Assert.isTrue(result == ERR, "Pop with NULL pointer should return ERR");
}

void test_stack_peek_with_null_returns_error(void) {
    Stack.clear();
    Stack.push(0x1234);

    int result = Stack.peek(NULL);
    Assert.isTrue(result == ERR, "Peek with NULL pointer should return ERR");
}
#endif

#if 1  // Region: Stack LIFO Order Tests
void test_stack_lifo_order(void) {
    Stack.clear();
    addr values[] = {0x1111, 0x2222, 0x3333, 0x4444};
    usize count = sizeof(values) / sizeof(values[0]);

    // Push values in order
    for (usize i = 0; i < count; i++) {
        Stack.push(values[i]);
    }

    Assert.isTrue(Stack.depth() == count, "Stack depth should match pushed count");

    // Pop values in reverse order (LIFO)
    for (usize i = count; i > 0; i--) {
        addr popped = 0;
        Stack.pop(&popped);
        Assert.isTrue(popped == values[i - 1], "Popped value should match LIFO order");
    }

    Assert.isTrue(Stack.is_empty(), "Stack should be empty after all pops");
}

void test_stack_multiple_push_peek_consistency(void) {
    Stack.clear();
    addr value1 = 0xAAAA, value2 = 0xBBBB, value3 = 0xCCCC;

    Stack.push(value1);
    Stack.push(value2);
    Stack.push(value3);

    addr peeked = 0;
    Stack.peek(&peeked);
    Assert.isTrue(peeked == value3, "Peek should return last pushed value");
    Assert.isTrue(Stack.depth() == 3, "Stack depth should be 3");

    Stack.pop(&peeked);
    Assert.isTrue(peeked == value3, "Pop should return last pushed value");
    Assert.isTrue(Stack.depth() == 2, "Stack depth should be 2");

    Stack.peek(&peeked);
    Assert.isTrue(peeked == value2, "Peek should now return second-to-last pushed value");
}
#endif

#if 1  // Region: Stack Capacity Tests
void test_stack_fill_to_capacity(void) {
    Stack.clear();
    // Stack capacity is 15 slots (120 bytes / 8 bytes per slot)
    usize capacity = 15;

    // Fill stack to capacity
    for (usize i = 0; i < capacity; i++) {
        int result = Stack.push((addr)i);
        Assert.isTrue(result == OK, "Push #%zu should succeed", i);
    }

    Assert.isTrue(Stack.is_full(), "Stack should be full");
    Assert.isTrue(Stack.depth() == capacity, "Stack depth should be %zu", capacity);
}

void test_stack_push_when_full_returns_error(void) {
    Stack.clear();
    usize capacity = 15;

    // Fill stack
    for (usize i = 0; i < capacity; i++) {
        Stack.push((addr)i);
    }

    // Try to push one more
    int result = Stack.push(0xFFFF);
    Assert.isTrue(result == ERR, "Push on full stack should return ERR");
    Assert.isTrue(Stack.depth() == capacity, "Stack depth should remain at capacity");
}

void test_stack_full_pop_push_cycle(void) {
    Stack.clear();
    usize capacity = 15;

    // Fill stack
    for (usize i = 0; i < capacity; i++) {
        Stack.push((addr)i);
    }
    Assert.isTrue(Stack.is_full(), "Stack should be full");

    // Pop one value
    addr popped = 0;
    Stack.pop(&popped);
    Assert.isFalse(Stack.is_full(), "Stack should not be full after pop");
    Assert.isTrue(Stack.depth() == capacity - 1, "Stack depth should be capacity - 1");

    // Push new value (should succeed)
    int result = Stack.push(0x9999);
    Assert.isTrue(result == OK, "Push after pop should succeed");
    Assert.isTrue(Stack.is_full(), "Stack should be full again");
}
#endif

#if 1  // Region: Stack Clear Tests
void test_stack_clear_empties_stack(void) {
    Stack.clear();
    Stack.push(0x1111);
    Stack.push(0x2222);
    Stack.push(0x3333);

    Assert.isTrue(Stack.depth() == 3, "Stack should have 3 items");

    Stack.clear();
    Assert.isTrue(Stack.is_empty(), "Stack should be empty after clear");
    Assert.isTrue(Stack.depth() == 0, "Stack depth should be 0 after clear");
}

void test_stack_reusable_after_clear(void) {
    Stack.clear();
    Stack.push(0xAAAA);
    Stack.push(0xBBBB);
    Stack.clear();

    // Push new values
    addr test_value = 0xCCCC;
    Stack.push(test_value);

    addr popped = 0;
    Stack.pop(&popped);
    Assert.isTrue(popped == test_value, "Stack should work correctly after clear");
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_stack_tests(void) {
    testset("Memory: NodeStack Operations", set_config, set_teardown);

    // Basic operations
    testcase("Stack: initialized empty", test_stack_initialized_empty);
    testcase("Stack: push single value", test_stack_push_single_value);
    testcase("Stack: push and pop value", test_stack_push_pop_value);
    testcase("Stack: peek preserves value", test_stack_peek_preserves_value);
    testcase("Stack: pop empty returns error", test_stack_pop_empty_returns_error);
    testcase("Stack: peek empty returns error", test_stack_peek_empty_returns_error);
    testcase("Stack: pop with NULL returns error", test_stack_pop_with_null_returns_error);
    testcase("Stack: peek with NULL returns error", test_stack_peek_with_null_returns_error);

    // LIFO order
    testcase("Stack: LIFO order", test_stack_lifo_order);
    testcase("Stack: multiple push/peek consistency", test_stack_multiple_push_peek_consistency);

    // Capacity
    testcase("Stack: fill to capacity", test_stack_fill_to_capacity);
    testcase("Stack: push when full returns error", test_stack_push_when_full_returns_error);
    testcase("Stack: full/pop/push cycle", test_stack_full_pop_push_cycle);

    // Clear
    testcase("Stack: clear empties stack", test_stack_clear_empties_stack);
    testcase("Stack: reusable after clear", test_stack_reusable_after_clear);
}
#endif
