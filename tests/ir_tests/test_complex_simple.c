_Complex float test_add(_Complex float a, _Complex float b) {
    return a + b;
}

int main(void) {
    _Complex float x = 1.0f;
    _Complex float y = 2.0f;
    _Complex float z = test_add(x, y);
    return 0;
}
