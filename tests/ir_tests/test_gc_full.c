// Test file for --gc-sections 
// unused_function and another_unused should be removed if GC works

int unused_function(void) {
    return 42;
}

int another_unused(int x) {
    return x * 2;
}

int main(void) {
    return 0;
}
