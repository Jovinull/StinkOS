/* libstink_math.c -- Software floating-point trig routines for StinkOS.
 *
 * Implements sin() and cos() using Taylor series approximation.
 * The input is in radians.
 */

#include "libstink.h"

#define M_PI 3.14159265358979323846

/* Normalize angle to [-PI, PI] */
static double normalize_angle(double x) {
    while (x > M_PI)  x -= 2.0 * M_PI;
    while (x < -M_PI) x += 2.0 * M_PI;
    return x;
}

double sin(double x) {
    x = normalize_angle(x);
    double result = 0.0;
    double term = x;
    double x_sq = x * x;
    int n = 1;

    /* Compute Taylor series up to x^11 for decent accuracy */
    while (n <= 11) {
        result += term;
        term *= -x_sq / ((n + 1) * (n + 2));
        n += 2;
    }
    return result;
}

double cos(double x) {
    x = normalize_angle(x);
    double result = 1.0;
    double term = 1.0;
    double x_sq = x * x;
    int n = 0;

    /* Compute Taylor series up to x^10 */
    while (n <= 10) {
        term *= -x_sq / ((n + 1) * (n + 2));
        result += term;
        n += 2;
    }
    return result;
}
