// SPDX-License-Identifier: GPL-3.0-only

#ifndef TYPETREE_DIRECTORY_INTERNAL_H
#define TYPETREE_DIRECTORY_INTERNAL_H

#include "io/serialized_file_directory.h"
#include "io/serialized_file_metadata_tail.h"
#include "io/typetree.h"

/* Materialize one ordinary row retained by a live directory over immutable
 * backing. big_endian must match that directory's prefix. The output must be
 * fresh or empty; failure frees all partial semantic allocations and leaves it
 * empty. This does not reread type/tree headers or choose optional fields. */
bool typetree_materialize_directory_type(
    TypeTreeType* type, const SerializedFileDirectoryTypeRow* row, bool big_endian);

/* The same output/lifetime contract for a genuine reference row retained by a
 * live tail. Preserve its reference identity and optional names; it has no
 * ordinary dependency trailer. This applies the existing semantic schema
 * policy after materializing the physically admitted tree and strings. */
bool typetree_materialize_metadata_tail_reference_type(
    TypeTreeType* type, const SerializedFileMetadataTailReferenceTypeRow* row, bool big_endian);

#endif
