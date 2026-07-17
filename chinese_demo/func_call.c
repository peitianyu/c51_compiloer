/* 21_func_call: 函数调用与返回 */

int add(int a, int b) {
    return a + b;
}

int sub(int a, int b) {
    return a - b;
}

int mul(int a, int b) {
    return a * b;
}

int main(void) {
    int x = add(10, 20);   /* 30 */
    int y = sub(50, 15);   /* 35 */
    return x + y;           /* 65 */
}
