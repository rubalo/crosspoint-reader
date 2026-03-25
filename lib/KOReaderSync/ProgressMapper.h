#pragma once
#include <Epub.h>

#include <memory>
#include <string>

/**
 * CrossPoint position representation.
 */
struct CrossPointPosition {
  int spineIndex;  // Current spine item (chapter) index
  int pageNumber;  // Current page within the spine item
  int totalPages;  // Total pages in the current spine item
};

/**
 * KOReader position representation.
 */
struct KOReaderPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
};

/**
 * Maps between CrossPoint and KOReader position formats.
 *
 * CrossPoint tracks position as (spineIndex, pageNumber).
 * KOReader uses XPath-like strings + percentage.
 *
 * Since CrossPoint discards HTML structure during parsing, we generate
 * synthetic XPath strings based on spine index, using percentage as the
 * primary sync mechanism.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to KOReader format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return KOReader position
   */
  static KOReaderPosition toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * Convert KOReader position to CrossPoint format.
   *
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                         int currentSpineIndex = -1, int totalPagesInCurrentSpine = 0);

 private:
  /**
   * Parse DocFragment index from KOReader XPath.
   * KOReader format: /body/DocFragment[N]/... where N is 1-based.
   * Returns 0-based spine index, or -1 if parsing fails.
   */
  static int parseDocFragmentIndex(const std::string& xpath);

  /**
   * Generate XPath for KOReader compatibility.
   * Format: /body/DocFragment[spineIndex+1]/body (1-based for KOReader)
   * Since CrossPoint doesn't preserve HTML structure, we rely on percentage for positioning.
   */
  static std::string generateXPath(int spineIndex, int pageNumber, int totalPages);
};
