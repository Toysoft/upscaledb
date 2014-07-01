/*
 * Copyright (C) 2005-2014 Christoph Rupp (chris@crupp.de).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HAM_ENV_HEADER_H__
#define HAM_ENV_HEADER_H__

#include <ham/hamsterdb.h>

#include "error.h"
#include "page.h"

namespace hamsterdb {

#include "packstart.h"

/**
 * the persistent file header
 */
typedef HAM_PACK_0 struct HAM_PACK_1
{
  /** magic cookie - always "ham\0" */
  ham_u8_t  _magic[4];

  /** version information - major, minor, rev, file */
  ham_u8_t  _version[4];

  /** reserved */
  ham_u64_t _reserved1;

  /** size of the page */
  ham_u32_t _page_size;

  /** maximum number of databases for this environment */
  ham_u16_t _max_databases;

  /** PRO: for storing journal compression algorithm */
  ham_u8_t _journal_compression;

  /** reserved */
  ham_u8_t _reserved3;

  /** blob id of the PageManager's state */
  ham_u64_t _pm_state;

  /*
   * following here:
   *
   * 1. the private data of the index btree(s)
   *      -> see get_btree_header()
   */
} HAM_PACK_2 PEnvironmentHeader;

#include "packstop.h"

class EnvironmentHeader
{
  public:
    // Constructor
    EnvironmentHeader(Device *device)
      : m_device(device), m_header_page(0) {
    }

    // TODO remove this; set page in constructor, remove device!
    void set_header_page(Page *page) {
      m_header_page = page;
    }

    // Sets the 'magic' field of a file header
    void set_magic(ham_u8_t m1, ham_u8_t m2, ham_u8_t m3, ham_u8_t m4) {
      get_header()->_magic[0] = m1;
      get_header()->_magic[1] = m2;
      get_header()->_magic[2] = m3;
      get_header()->_magic[3] = m4;
    }

    // Returns true if the magic matches
    bool verify_magic(ham_u8_t m1, ham_u8_t m2, ham_u8_t m3, ham_u8_t m4) {
      if (get_header()->_magic[0] != m1)
        return (false);
      if (get_header()->_magic[1] != m2)
        return (false);
      if (get_header()->_magic[2] != m3)
        return (false);
      if (get_header()->_magic[3] != m4)
        return (false);
      return (true);
    }

    // Returns byte |i| of the 'version'-header
    // TODO use a logical structure 'Version'
    ham_u8_t get_version(ham_u32_t idx) {
      return (get_header()->_version[idx]);
    }

    // Sets the version of a file header
    // TODO use a logical structure 'Version'
    void set_version(ham_u8_t a, ham_u8_t b, ham_u8_t c, ham_u8_t d) {
      get_header()->_version[0] = a;
      get_header()->_version[1] = b;
      get_header()->_version[2] = c;
      get_header()->_version[3] = d;
    }

    // Returns get the maximum number of databases for this file
    ham_u16_t get_max_databases() {
      return (get_header()->_max_databases);
    }

    // Sets the maximum number of databases for this file
    void set_max_databases(ham_u16_t md) {
      get_header()->_max_databases = md;
    }

    // Returns the page size from the header page
    ham_u32_t get_page_size() {
      return (get_header()->_page_size);
    }

    // Sets the page size in the header page
    void set_page_size(ham_u32_t ps) {
      get_header()->_page_size = ps;
    }

    // Returns the PageManager's blob id
    ham_u64_t get_page_manager_blobid() {
      return (get_header()->_pm_state);
    }

    // Sets the page size in the header page
    void set_page_manager_blobid(ham_u64_t blobid) {
      get_header()->_pm_state = blobid;
    }

    // Returns the Journal compression configuration
    int get_journal_compression(int *level) {
      *level = get_header()->_journal_compression & 0x0f;
      return (get_header()->_journal_compression >> 4);
    }

    // Sets the Journal compression configuration
    void set_journal_compression(int algorithm, int level) {
      get_header()->_journal_compression = (algorithm << 4) | level;
    }

    // Returns the header page with persistent configuration settings
    Page *get_header_page() {
      return (m_header_page);
    }

  private:
    // Returns a pointer to the header data
    PEnvironmentHeader *get_header() {
      return ((PEnvironmentHeader *)(m_header_page->get_payload()));
    }

    // The device
    Device *m_device;

    // The header page of the Environment
    Page *m_header_page;
};

} // namespace hamsterdb

#endif /* HAM_ENV_HEADER_H__ */
