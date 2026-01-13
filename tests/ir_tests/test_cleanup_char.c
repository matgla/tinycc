extern int printf(const char*, ...);

void check_oh_i(char *oh_i)
{
    printf("c: %c (0x%02x)\n", *oh_i, (unsigned char)*oh_i);
}

int main()
{
    {
	__attribute__ ((__cleanup__(check_oh_i))) char oh_i = 'o', o = 'a';
    }
    return 0;
}
