#include "ProgressMapper.h"

#include <Logging.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;

  // Calculate page progress within current spine item
  float intraSpineProgress = 0.0f;
  if (pos.totalPages > 0) {
    intraSpineProgress = static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages);
  }

  // Calculate overall book progress (0.0-1.0)
  result.percentage = epub->calculateProgress(pos.spineIndex, intraSpineProgress);

  // Generate XPath with estimated paragraph position based on page
  result.xpath = generateXPath(pos.spineIndex, pos.pageNumber, pos.totalPages);

  // Get chapter info for logging
  const int tocIndex = epub->getTocIndexForSpineIndex(pos.spineIndex);
  const std::string chapterName = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : "unknown";

  LOG_DBG("ProgressMapper", "CrossPoint -> KOReader: chapter='%s', page=%d/%d -> %.2f%% at %s", chapterName.c_str(),
          pos.pageNumber, pos.totalPages, result.percentage * 100, result.xpath.c_str());

  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition result;
  result.spineIndex = 0;
  result.pageNumber = 0;
  result.totalPages = 0;

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return result;
  }

  const int spineCount = epub->getSpineItemsCount();

  // Try to extract spine index from XPath DocFragment.
  // Formats: "#_doc_fragment_N" (0-based) or "/body/DocFragment[N]/..." (1-based)
  int xpathSpineIndex = parseDocFragmentIndex(koPos.xpath);
  bool spineFromXPath = false;
  bool isFragmentStart = false;

  if (xpathSpineIndex >= 0 && xpathSpineIndex < spineCount) {
    result.spineIndex = xpathSpineIndex;
    spineFromXPath = true;
    // If the XPath is just "#_doc_fragment_N" with no sub-element path,
    // the position is at the start of the fragment (page 0).
    isFragmentStart = (koPos.xpath.find('#') != std::string::npos && koPos.xpath.find('/') == std::string::npos);
    LOG_DBG("ProgressMapper", "Spine %d from XPath (start=%s)", xpathSpineIndex, isFragmentStart ? "yes" : "no");
  } else {
    // Fallback: use percentage to find spine item
    const size_t targetBytes = static_cast<size_t>(bookSize * koPos.percentage);
    bool spineFound = false;
    for (int i = 0; i < spineCount; i++) {
      const size_t cumulativeSize = epub->getCumulativeSpineItemSize(i);
      if (cumulativeSize >= targetBytes) {
        result.spineIndex = i;
        spineFound = true;
        break;
      }
    }
    if (!spineFound && spineCount > 0) {
      result.spineIndex = spineCount - 1;
    }
    LOG_DBG("ProgressMapper", "Spine %d from percentage fallback (%.2f%%)", result.spineIndex, koPos.percentage * 100);
  }

  // Estimate page number within the spine item using percentage
  if (result.spineIndex < spineCount) {
    const size_t prevCumSize = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
    const size_t currentCumSize = epub->getCumulativeSpineItemSize(result.spineIndex);
    const size_t spineSize = currentCumSize - prevCumSize;

    int estimatedTotalPages = 0;

    // If we are in the same spine, use the known total pages
    if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
      estimatedTotalPages = totalPagesInCurrentSpine;
    }
    // Otherwise try to estimate based on density from current spine
    else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
      const size_t prevCurrCumSize =
          (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
      const size_t currCumSize = epub->getCumulativeSpineItemSize(currentSpineIndex);
      const size_t currSpineSize = currCumSize - prevCurrCumSize;

      if (currSpineSize > 0) {
        float ratio = static_cast<float>(spineSize) / static_cast<float>(currSpineSize);
        estimatedTotalPages = static_cast<int>(totalPagesInCurrentSpine * ratio);
        if (estimatedTotalPages < 1) estimatedTotalPages = 1;
      }
    }

    result.totalPages = estimatedTotalPages;

    // Use percentage to estimate position within the spine item,
    // unless the XPath indicates the start of a fragment (page 0).
    if (!isFragmentStart && spineSize > 0 && estimatedTotalPages > 0) {
      // Calculate what fraction of this spine item the percentage corresponds to
      const float spineStartPct = static_cast<float>(prevCumSize) / static_cast<float>(bookSize);
      const float spineEndPct = static_cast<float>(currentCumSize) / static_cast<float>(bookSize);
      const float spineRange = spineEndPct - spineStartPct;

      float intraSpineProgress = 0.0f;
      if (spineRange > 0.0f) {
        intraSpineProgress = (koPos.percentage - spineStartPct) / spineRange;
        intraSpineProgress = std::max(0.0f, std::min(1.0f, intraSpineProgress));
      }

      result.pageNumber = static_cast<int>(intraSpineProgress * estimatedTotalPages);
      result.pageNumber = std::max(0, std::min(result.pageNumber, estimatedTotalPages - 1));
    }
  }

  LOG_DBG("ProgressMapper", "KOReader -> CrossPoint: %.2f%% at %s -> spine=%d, page=%d (xpath=%s)", koPos.percentage * 100,
          koPos.xpath.c_str(), result.spineIndex, result.pageNumber, spineFromXPath ? "yes" : "no");

  return result;
}

int ProgressMapper::parseDocFragmentIndex(const std::string& xpath) {
  // KOReader sends progress in two possible formats:
  //   1. XPath: "/body/DocFragment[N]/..." where N is 1-based
  //   2. Anchor: "#_doc_fragment_N" where N is 0-based
  // Returns 0-based spine index, or -1 if parsing fails.

  const char* str = xpath.c_str();

  // Try anchor format first: "#_doc_fragment_N" (0-based)
  const char* anchor = strstr(str, "#_doc_fragment_");
  if (anchor) {
    const char* numStart = anchor + 15;  // strlen("#_doc_fragment_")
    char* numEnd = nullptr;
    long idx = strtol(numStart, &numEnd, 10);
    if (numEnd != numStart && idx >= 0) {
      return static_cast<int>(idx);  // Already 0-based
    }
  }

  // Try XPath format: "/body/DocFragment[N]/" (1-based)
  const char* prefix = "DocFragment[";
  const char* pos = strstr(str, prefix);
  if (pos) {
    const char* numStart = pos + 12;  // strlen("DocFragment[")
    char* numEnd = nullptr;
    long docFragment = strtol(numStart, &numEnd, 10);
    if (numEnd != numStart && *numEnd == ']' && docFragment >= 1) {
      return static_cast<int>(docFragment - 1);  // Convert 1-based to 0-based
    }
  }

  return -1;
}

std::string ProgressMapper::generateXPath(int spineIndex, int pageNumber, int totalPages) {
  // KOReader uses 1-based DocFragment indices (XPath standard)
  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
}
