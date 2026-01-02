int main() {
    long long x = 0x123456789ABCDEF0LL;  // 64-bit literal, requires LDRD
    int y = 0x12345678;                   // 32-bit literal
    long long z = 0xFEDCBA9876543210LL;  // another 64-bit literal
    return (int)(x + z) + y;
}
