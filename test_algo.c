#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// Mimics the wasm print_f64 algorithm exactly
void test_print_f64(double x) {
    char buf[6]; // buf[0..5]
    char out[48]; // output buffer
    int o = 0;   // output pointer (index into out)
    
    int is_neg = x < 0.0;
    if (x != x) { printf("nan"); return; }
    if (is_neg) x = -x;
    uint64_t bits; memcpy(&bits, &x, 8);
    if (bits == 0x7FF0000000000000ULL) { 
        if (is_neg) printf("-inf"); else printf("inf"); 
        return; 
    }
    if (x == 0.0) {
        if (is_neg) printf("-0"); else printf("0");
        return;
    }
    
    int e = 0;
    double pow = 1.0;
    while (pow * 10.0 <= x) { pow *= 10.0; e++; }
    while (pow > x) { pow /= 10.0; e--; }
    
    fprintf(stderr, "DEBUG: x=%.17g e=%d pow=%.17g\n", x, e, pow);
    
    double r = x / pow;
    
    fprintf(stderr, "DEBUG: r=%.17g (before r>=10 check)\n", r);
    
    // NOTE: the wasm code has: if (pow >= 10.0) r = pow + 1e-9
    // This is WRONG! It should be: if (r >= 10.0) r = 9.999999999
    // Let me test BOTH versions
    
    // Version A: what the wasm code does (checking pow >= 10 instead of r >= 10)
    if (pow >= 10.0) r = pow + 1e-9;
    
    fprintf(stderr, "DEBUG: r=%.17g (AFTER buggy check: pow>=10 -> r=pow+eps)\n", r);
    
    int carry = 0;
    for (int i = 0; i < 6; i++) {
        int d = (int)trunc(r + 1e-9);
        if (d >= 10) {
            d = 1;
            e++;
            carry = 1;
        }
        buf[i] = (char)(d + 48);
        r = (r - (double)d) * 10.0;
    }
    
    fprintf(stderr, "DEBUG: buf='%c%c%c%c%c%c' e=%d carry=%d\n", 
            buf[0],buf[1],buf[2],buf[3],buf[4],buf[5], e, carry);
    
    if (carry) {
        buf[0] = '1';
        for (int j = 1; j < 6; j++) buf[j] = '0';
    }
    
    int sci = (e >= 6 || e < -4);
    
    if (sci) {
        out[o++] = buf[0];
        out[o++] = '.';
        for (int j = 1; j < 6; j++) { out[o++] = buf[j]; }
        while (o > 2 && out[o-1] == '0') o--;
        if (out[o-1] == '.') o--;
        out[o++] = 'e';
        if (e >= 0) { out[o++] = '+'; } else { out[o++] = '-'; e = -e; }
        if (e < 10) { out[o++] = (char)(e + 48); }
        else { out[o++] = (char)(e/10 + 48); out[o++] = (char)(e%10 + 48); }
    } else {
        if (e >= 0) {
            for (int i = 0; i <= e; i++) out[o++] = buf[i];
            if (e < 5) {
                out[o++] = '.';
                for (int i = e + 1; i < 6; i++) out[o++] = buf[i];
                while (o > 0 && out[o-1] == '0') o--;
                if (o > 0 && out[o-1] == '.') o--;
            }
        } else {
            out[o++] = '0';
            out[o++] = '.';
            for (int j = 0; j < -e - 1; j++) out[o++] = '0';
            for (int j = 0; j < 6; j++) out[o++] = buf[j];
            while (o > 0 && out[o-1] == '0') o--;
            if (o > 0 && out[o-1] == '.') o--;
        }
    }
    
    printf("%.*s", o, out);
}

int main() {
    double tests[] = {1.0, 10.0, 0.5, 1.5, 42.0, 3.14159, 0.001};
    for (int i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        fprintf(stderr, "\n=== Testing %g ===\n", tests[i]);
        test_print_f64(tests[i]);
        printf(" (expected %%g: %g)\n", tests[i]);
    }
    return 0;
}
