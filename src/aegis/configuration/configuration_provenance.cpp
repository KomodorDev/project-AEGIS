// Purpose: expose stable display and exact metadata-revision lookup for configuration provenance.

#include "aegis/configuration/configuration_provenance.hpp"

#include <algorithm>
#include <tuple>

namespace aegis::configuration {

// --------------------------------------------------------
// Formatting reuses the digest's fixed-width lowercase representation without hashing it again.
std::string ConfigurationFingerprint::to_hex() const {
  const model::Sha256Hex hexadecimal = model::sha256_hex(bytes_);
  return std::string{hexadecimal.data(), hexadecimal.size()};
}

// --------------------------------------------------------
// Revision entries inherit canonical venue/instrument order from startup metadata validation.
const model::InstrumentMetadataRevision* ConfigurationProvenance::find_instrument_metadata_revision(
    const model::VenueId& venue_id, const model::InstrumentId& instrument_id) const noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Locate the composite key without allocating or maintaining a secondary index.
  const auto key = std::tie(venue_id, instrument_id);
  const auto found =
      std::lower_bound(instrument_metadata_revisions_.begin(), instrument_metadata_revisions_.end(),
                       key, [](const InstrumentMetadataRevisionEntry& entry, const auto& target) {
                         return std::tie(entry.venue_id, entry.instrument_id) < target;
                       });

  // ++++++++++++++++++++++++++++++++++++++++
  // Return a revision only for an exact composite-key match.
  if (found == instrument_metadata_revisions_.end() || found->venue_id != venue_id ||
      found->instrument_id != instrument_id) {
    return nullptr;
  }
  return &found->revision;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::configuration
