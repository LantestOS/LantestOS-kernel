#pragma once
#define HUGE_VAL (1.0 / 0.0)
#define INFINITY (1.0 / 0.0)
#define NAN (0.0 / 0.0)
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double sqrt(double x);
double pow(double x, double y);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log10(double x);
int isinf(double x);
int isnan(double x);
