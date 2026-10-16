#pragma once

class BarcodeScanner;

int run_camera(BarcodeScanner &scanner);
int run_test_image(const char *path, BarcodeScanner &scanner);
