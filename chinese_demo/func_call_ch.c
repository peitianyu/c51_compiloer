/* 21_func_call: 函数调用与返回 */

int 加(int a, int b) {
    return a + b;
}

int 减(int a, int b) {
    return a - b;
}

int 乘(int a, int b) {
    return a * b;
}

int main(void) {
    int 甲 = 加(10, 20);   /* 30 */
    int 乙 = 减(50, 15);   /* 35 */
    return 甲 + 乙;        /* 65 */
}
