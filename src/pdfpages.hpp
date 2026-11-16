#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Finds the page pictures in a scanned PDF (a manual): for each page, in page order, where its
// JPEG picture is in the file (offset and size). Scans are usually saved as one JPEG per page, so
// the pages can be shown without a PDF reader. False if the pages aren't JPEG pictures (some PDFs
// use JPEG 2000 or black-and-white formats, or are made of text): show them some other way.
struct PdfPicture {
    size_t offset = 0, size = 0;  // size 0: this page has no JPEG picture
};
bool pdfJpegPages(const std::vector<uint8_t>& pdf, std::vector<PdfPicture>& pages);
