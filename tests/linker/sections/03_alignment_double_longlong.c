/* Wider scalar types must be placed at their naturally aligned offsets
 * within .data even when preceded by narrower objects: the char forces
 * 7 bytes of padding before the double, and the long long array follows
 * at an 8-byte boundary too. */
char c = 1;
double d = 3.14;
long long arr[4] = {1, 2, 3, 4};
