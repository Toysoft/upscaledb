/*
 * Copyright (C) 2005-2014 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * NOTICE: All information contained herein is, and remains the property
 * of Christoph Rupp and his suppliers, if any. The intellectual and
 * technical concepts contained herein are proprietary to Christoph Rupp
 * and his suppliers and may be covered by Patents, patents in process,
 * and are protected by trade secret or copyright law. Dissemination of
 * this information or reproduction of this material is strictly forbidden
 * unless prior written permission is obtained from Christoph Rupp.
 */

#include "config.h"

#include "version.h"
#include "db.h"
#include "btree_index.h"
#include "btree_stats.h"
#include "device_factory.h"
#include "blob_manager_factory.h"
#include "page_manager.h"
#include "journal.h"
#include "txn.h"
#include "txn_local.h"
#include "env_local.h"
#include "cursor.h"
#include "txn_cursor.h"
#include "compressor_factory.h"

using namespace hamsterdb;

namespace hamsterdb {

PBtreeHeader *
LocalEnvironment::get_btree_descriptor(int i)
{
  PBtreeHeader *d = (PBtreeHeader *)
        (m_header->get_header_page()->get_payload()
            + sizeof(PEnvironmentHeader));
  return (d + i);
}

LocalEnvironment::LocalEnvironment()
  : Environment(), m_header(0), m_device(0), m_changeset(this),
    m_blob_manager(0), m_page_manager(0), m_journal(0), 
    m_encryption_enabled(false), m_journal_compression(0), m_page_size(0)
{
}

LocalEnvironment::~LocalEnvironment()
{
  (void)close(HAM_AUTO_CLEANUP);
}

ham_status_t
LocalEnvironment::create(const char *filename, ham_u32_t flags,
            ham_u32_t mode, size_t page_size, ham_u64_t cache_size,
            ham_u16_t max_databases, ham_u64_t file_size_limit)
{
  if (flags & HAM_IN_MEMORY)
    flags |= HAM_DISABLE_RECLAIM_INTERNAL;
  set_flags(flags);

  if (filename)
    m_filename = filename;
  m_file_mode = mode;
  m_page_size = page_size;

  /* initialize the device if it does not yet exist */
  m_blob_manager = BlobManagerFactory::create(this, flags);
  m_device = DeviceFactory::create(this, flags, file_size_limit);
  if (flags & HAM_ENABLE_TRANSACTIONS)
    m_txn_manager = new LocalTransactionManager(this);

  /* create the file */
  m_device->create(filename, flags, mode);

  /* create the configuration object */
  m_header = new EnvironmentHeader(m_device);

  /* allocate the header page */
  {
    Page *page = new Page(this);
    page->allocate(Page::kTypeHeader, m_page_size);
    memset(page->get_data(), 0, m_page_size);
    page->set_type(Page::kTypeHeader);
    m_header->set_header_page(page);

    /* initialize the header; hamsterdb pro always sets the msb of the
     * file version */
    m_header->set_magic('H', 'A', 'M', '\0');
    m_header->set_version(HAM_VERSION_MAJ, HAM_VERSION_MIN, HAM_VERSION_REV,
            HAM_FILE_VERSION | 0x80);
    m_header->set_page_size(m_page_size);
    m_header->set_max_databases(max_databases);

    page->set_dirty(true);
  }

  /* load page manager after setting up the blobmanager and the device! */
  m_page_manager = new PageManager(this,
                        flags & HAM_CACHE_UNLIMITED
                            ? 0xffffffffffffffffull
                            : cache_size);

  /* create a logfile and a journal (if requested) */
  if (get_flags() & HAM_ENABLE_RECOVERY) {
    m_journal = new Journal(this);
    m_journal->create();
  }

  /* Now that the header was created we can finally store the compression
   * information */
  if (m_journal_compression)
    m_header->set_journal_compression(m_journal_compression);

  /* flush the header page - this will write through disk if logging is
   * enabled */
  if (get_flags() & HAM_ENABLE_RECOVERY)
    m_page_manager->flush_page(m_header->get_header_page());

  return (0);
}

ham_status_t
LocalEnvironment::open(const char *filename, ham_u32_t flags,
            ham_u64_t cache_size, ham_u64_t file_size_limit)
{
  ham_status_t st = 0;

  /* initialize the device if it does not yet exist */
  m_blob_manager = BlobManagerFactory::create(this, flags);
  m_device = DeviceFactory::create(this, flags, file_size_limit);

  if (filename)
    m_filename = filename;
  set_flags(flags);

  /* open the file */
  m_device->open(filename, flags);

  if (flags & HAM_ENABLE_TRANSACTIONS)
    m_txn_manager = new LocalTransactionManager(this);

  /* create the configuration object */
  m_header = new EnvironmentHeader(m_device);

  /*
   * read the database header
   *
   * !!!
   * now this is an ugly problem - the database header spans one page, but
   * what's the size of this page? chances are good that it's the default
   * page-size, but we really can't be sure.
   *
   * read 512 byte and extract the "real" page size, then read
   * the real page.
   */
  {
    Page *page = 0;
    ham_u8_t hdrbuf[512];
    Page fakepage(this);

    /*
     * in here, we're going to set up a faked headerpage for the
     * duration of this call; BE VERY CAREFUL: we MUST clean up
     * at the end of this section or we'll be in BIG trouble!
     */
    fakepage.set_data((PPageData *)hdrbuf);
    m_header->set_header_page(&fakepage);

    /*
     * now fetch the header data we need to get an estimate of what
     * the database is made of really.
     */
    m_device->read(0, hdrbuf, sizeof(hdrbuf));

    m_page_size = m_header->get_page_size();

    /** check the file magic */
    if (!m_header->verify_magic('H', 'A', 'M', '\0')) {
      ham_log(("invalid file type"));
      st  =  HAM_INV_FILE_HEADER;
      goto fail_with_fake_cleansing;
    }

    /* check the database version; everything with a different file version
     * is incompatible */
    if (m_header->get_version(3) == HAM_FILE_VERSION) {
      // this file was created with the APL version; upgrade it to
      // the commercial license by setting the msb of the file version
      if ((get_flags() & HAM_READ_ONLY) == 0) {
        m_header->set_version(HAM_VERSION_MAJ, HAM_VERSION_MIN, HAM_VERSION_REV,
              HAM_FILE_VERSION | 0x80);
        m_header->get_header_page()->set_dirty(true);
        m_device->write(0, hdrbuf, sizeof(hdrbuf));
      }
    }
    else if (m_header->get_version(3) != (HAM_FILE_VERSION | 0x80)) {
      ham_log(("invalid file version"));
      st = HAM_INV_FILE_VERSION;
      goto fail_with_fake_cleansing;
    }
    else if (m_header->get_version(0) == 1 &&
      m_header->get_version(1) == 0 &&
      m_header->get_version(2) <= 9) {
      ham_log(("invalid file version; < 1.0.9 is not supported"));
      st = HAM_INV_FILE_VERSION;
      goto fail_with_fake_cleansing;
    }

    st = 0;

fail_with_fake_cleansing:

    /* undo the headerpage fake first! */
    fakepage.set_data(0);
    m_header->set_header_page(0);

    /* exit when an error was signaled */
    if (st) {
      if (m_device->is_open())
        m_device->close();
      delete m_device;
      m_device = 0;
      return (st);
    }

    /* now read the "real" header page and store it in the Environment */
    page = new Page(this);
    page->fetch(0);
    m_header->set_header_page(page);
  }

  /* Now that the header page was fetched we can retrieve the compression
   * information */
  m_journal_compression = m_header->get_journal_compression();

  /* load page manager after setting up the blobmanager and the device! */
  m_page_manager = new PageManager(this,
                        flags & HAM_CACHE_UNLIMITED
                            ? 0xffffffffffffffffull
                            : cache_size);

  /*
   * open the logfile and check if we need recovery. first open the
   * (physical) log and re-apply it. afterwards to the same with the
   * (logical) journal.
   */
  if (get_flags() & HAM_ENABLE_RECOVERY)
    recover(flags);

  /* load the state of the PageManager */
  if (m_header->get_page_manager_blobid() != 0) {
    m_page_manager->load_state(m_header->get_page_manager_blobid());
    if (get_flags() & HAM_ENABLE_RECOVERY)
      get_changeset().clear();
  }

  return (0);
}

ham_status_t
LocalEnvironment::rename_db(ham_u16_t oldname, ham_u16_t newname,
    ham_u32_t flags)
{
  ham_status_t st = 0;

  /*
   * check if a database with the new name already exists; also search
   * for the database with the old name
   */
  ham_u16_t max = m_header->get_max_databases();
  ham_u16_t slot = max;
  ham_assert(max > 0);
  for (ham_u16_t dbi = 0; dbi < max; dbi++) {
    ham_u16_t name = get_btree_descriptor(dbi)->get_dbname();
    if (name == newname)
      return (HAM_DATABASE_ALREADY_EXISTS);
    if (name == oldname)
      slot = dbi;
  }

  if (slot == max)
    return (HAM_DATABASE_NOT_FOUND);

  /* replace the database name with the new name */
  get_btree_descriptor(slot)->set_dbname(newname);
  mark_header_page_dirty();

  /* if the database with the old name is currently open: notify it */
  Environment::DatabaseMap::iterator it = get_database_map().find(oldname);
  if (it != get_database_map().end()) {
    Database *db = it->second;
    it->second->set_name(newname);
    get_database_map().erase(oldname);
    get_database_map().insert(DatabaseMap::value_type(newname, db));
  }

  /* flush the header page if logging is enabled */
  if (get_flags() & HAM_ENABLE_RECOVERY)
    get_changeset().flush(get_incremented_lsn());

  return (st);
}

ham_status_t
LocalEnvironment::erase_db(ham_u16_t name, ham_u32_t flags)
{
  /* check if this database is still open */
  if (get_database_map().find(name) != get_database_map().end())
    return (HAM_DATABASE_ALREADY_OPEN);

  /*
   * if it's an in-memory environment then it's enough to purge the
   * database from the environment header
   */
  if (get_flags() & HAM_IN_MEMORY) {
    for (ham_u16_t dbi = 0; dbi < m_header->get_max_databases(); dbi++) {
      PBtreeHeader *desc = get_btree_descriptor(dbi);
      if (name == desc->get_dbname()) {
        desc->set_dbname(0);
        return (0);
      }
    }
    return (HAM_DATABASE_NOT_FOUND);
  }

  /* temporarily load the database */
  LocalDatabase *db;
  ham_status_t st = open_db((Database **)&db, name, 0, 0);
  if (st)
    return (st);

  /* logging enabled? then the changeset HAS to be empty */
#ifdef HAM_DEBUG
  if (get_flags() & HAM_ENABLE_RECOVERY)
    ham_assert(get_changeset().is_empty());
#endif

  /*
   * delete all blobs and extended keys, also from the cache and
   * the extkey-cache
   *
   * also delete all pages and move them to the freelist; if they're
   * cached, delete them from the cache
   */
  db->erase_me();

  /* now set database name to 0 and set the header page to dirty */
  for (ham_u16_t dbi = 0; dbi < m_header->get_max_databases(); dbi++) {
    PBtreeHeader *desc = get_btree_descriptor(dbi);
    if (name == desc->get_dbname()) {
      desc->set_dbname(0);
      break;
    }
  }

  mark_header_page_dirty();

  /* if logging is enabled: flush the changeset because the header page
   * was modified */
  if (get_flags() & HAM_ENABLE_RECOVERY)
    get_changeset().flush(get_incremented_lsn());

  (void)ham_db_close((ham_db_t *)db, HAM_DONT_LOCK);

  return (0);
}

ham_status_t
LocalEnvironment::get_database_names(ham_u16_t *names, ham_u32_t *count)
{
  ham_u16_t name;
  ham_u32_t i = 0;
  ham_u32_t max_names = 0;

  max_names = *count;
  *count = 0;

  /* copy each database name to the array */
  ham_assert(m_header->get_max_databases() > 0);
  for (i = 0; i<m_header->get_max_databases(); i++) {
    name = get_btree_descriptor(i)->get_dbname();
    if (name == 0)
      continue;

    if (*count >= max_names)
      return (HAM_LIMITS_REACHED);

    names[(*count)++] = name;
  }

  return 0;
}

ham_status_t
LocalEnvironment::close(ham_u32_t flags)
{
  ham_status_t st;
  Device *device = get_device();

  /* flush all committed transactions */
  if (get_txn_manager())
    get_txn_manager()->flush_committed_txns();

  /* close all databases */
  Environment::DatabaseMap::iterator it = get_database_map().begin();
  while (it != get_database_map().end()) {
    Environment::DatabaseMap::iterator it2 = it; it++;
    Database *db = it2->second;
    if (flags & HAM_AUTO_CLEANUP)
      st = ham_db_close((ham_db_t *)db, flags | HAM_DONT_LOCK);
    else
      st = db->close(flags);
    if (st)
      return (st);
  }

  // store the state of the PageManager
  if (m_page_manager
      && (get_flags() & HAM_IN_MEMORY) == 0
      && (get_flags() & HAM_READ_ONLY) == 0) {
    ham_u64_t new_blobid = m_page_manager->store_state();
    Page *hdrpage = get_header()->get_header_page();
    if (new_blobid != get_header()->get_page_manager_blobid()) {
      get_header()->set_page_manager_blobid(new_blobid);
      hdrpage->set_dirty(true);
    }
    if (get_flags() & HAM_ENABLE_RECOVERY) {
      if (hdrpage->is_dirty())
        get_changeset().add_page(hdrpage);
      //if (m_journal && (flags & HAM_DONT_CLEAR_LOG) == 0)
      if (!get_changeset().is_empty())
        get_changeset().flush(get_incremented_lsn());
    }
  }

  /* flush all committed transactions */
  if (m_txn_manager)
    get_txn_manager()->flush_committed_txns();

  /* flush all pages and the freelist, reduce the file size */
  if (m_page_manager)
    m_page_manager->close();

  /* if we're not in read-only mode, and not an in-memory-database,
   * and the dirty-flag is true: flush the page-header to disk
   */
  if (m_header && m_header->get_header_page() && !(get_flags() & HAM_IN_MEMORY)
      && get_device() && get_device()->is_open()
      && (!(get_flags() & HAM_READ_ONLY))) {
    m_header->get_header_page()->flush();
  }

  /* now delete the TransactionManager (it's required during
   * PageManager::close(), therefore it cannot be deleted earlier) */
  if (m_txn_manager) {
    delete m_txn_manager;
    m_txn_manager = 0;
  }

  /* close the page manager (includes cache and freelist) */
  if (m_page_manager) {
    delete m_page_manager;
    m_page_manager = 0;
  }

  /* close the header page */
  if (m_header && m_header->get_header_page()) {
    Page *page = m_header->get_header_page();
    ham_assert(device);
    if (page->get_data())
      device->free_page(page);
    delete page;
    m_header->set_header_page(0);
  }

  /* close the device */
  if (device) {
    if (device->is_open()) {
      if (!(get_flags() & HAM_READ_ONLY))
        device->flush();
      device->close();
    }
    delete device;
    m_device = 0;
  }

  /* close the log and the journal */
  if (m_journal) {
    m_journal->close(!!(flags & HAM_DONT_CLEAR_LOG));
    delete m_journal;
    m_journal = 0;
  }

  if (m_blob_manager) {
    delete m_blob_manager;
    m_blob_manager = 0;
  }

  if (m_header != 0) {
    delete m_header;
    m_header = 0;
  }

  return (0);
}

ham_status_t
LocalEnvironment::get_parameters(ham_parameter_t *param)
{
  ham_parameter_t *p = param;

  if (p) {
    for (; p->name; p++) {
      switch (p->name) {
      case HAM_PARAM_CACHE_SIZE:
        p->value = get_page_manager()->get_cache_capacity();
        break;
      case HAM_PARAM_PAGE_SIZE:
        p->value = m_page_size;
        break;
      case HAM_PARAM_MAX_DATABASES:
        p->value = m_header->get_max_databases();
        break;
      case HAM_PARAM_FLAGS:
        p->value = get_flags();
        break;
      case HAM_PARAM_FILEMODE:
        p->value = get_file_mode();
        break;
      case HAM_PARAM_FILENAME:
        if (get_filename().size())
          p->value = (ham_u64_t)(PTR_TO_U64(get_filename().c_str()));
        else
          p->value = 0;
        break;
      case HAM_PARAM_LOG_DIRECTORY:
        if (get_log_directory().size())
          p->value = (ham_u64_t)(PTR_TO_U64(get_log_directory().c_str()));
        else
          p->value = 0;
        break;
      case HAM_PARAM_JOURNAL_COMPRESSION:
        p->value = m_journal_compression;
        break;
      default:
        ham_trace(("unknown parameter %d", (int)p->name));
        return (HAM_INV_PARAMETER);
      }
    }
  }

  return (0);
}

ham_status_t
LocalEnvironment::flush(ham_u32_t flags)
{
  Device *device = get_device();

  /* never flush an in-memory-database */
  if (get_flags() & HAM_IN_MEMORY)
    return (0);

  /* flush all committed transactions */
  if (get_txn_manager())
    get_txn_manager()->flush_committed_txns();

  /* flush the header page, if necessary */
  if (m_header->get_header_page()->is_dirty())
    get_page_manager()->flush_page(m_header->get_header_page());

  /* flush all open pages to disk */
  get_page_manager()->flush_all_pages(true);

  /* flush the device - this usually causes a fsync() */
  device->flush();

  return (HAM_SUCCESS);
}

ham_status_t
LocalEnvironment::create_db(Database **pdb, ham_u16_t dbname,
            ham_u32_t flags, const ham_parameter_t *param)
{
  ham_u16_t key_type = HAM_TYPE_BINARY;
  ham_u32_t key_size = HAM_KEY_SIZE_UNLIMITED;
  ham_u32_t rec_size = HAM_RECORD_SIZE_UNLIMITED;
  ham_u32_t record_compressor = HAM_COMPRESSOR_NONE;
  ham_u32_t key_compressor = HAM_COMPRESSOR_NONE;
  ham_u16_t dbi;
  std::string logdir;

  *pdb = 0;

  if (get_flags() & HAM_READ_ONLY) {
    ham_trace(("cannot create database in a read-only environment"));
    return (HAM_WRITE_PROTECTED);
  }

  if (param) {
    for (; param->name; param++) {
      switch (param->name) {
        case HAM_PARAM_RECORD_COMPRESSION:
          if (!CompressorFactory::is_available(param->value)) {
            ham_trace(("unknown algorithm for record compression"));
            return (HAM_INV_PARAMETER);
          }
          record_compressor = (int)param->value;
          break;
        case HAM_PARAM_KEY_COMPRESSION:
          if (!CompressorFactory::is_available(param->value)) {
            ham_trace(("unknown algorithm for key compression"));
            return (HAM_INV_PARAMETER);
          }
          key_compressor = (int)param->value;
          break;
        case HAM_PARAM_KEY_TYPE:
          key_type = (ham_u16_t)param->value;
          break;
        case HAM_PARAM_KEY_SIZE:
          if (param->value != 0) {
            if (param->value > 0xffff) {
              ham_trace(("invalid key size %u - must be < 0xffff"));
              return (HAM_INV_KEY_SIZE);
            }
            key_size = (ham_u16_t)param->value;
            if (flags & HAM_RECORD_NUMBER) {
              if (key_size > 0 && key_size < sizeof(ham_u64_t)) {
                ham_trace(("invalid key size %u - must be 8 for "
                           "HAM_RECORD_NUMBER databases", (unsigned)key_size));
                return (HAM_INV_KEY_SIZE);
              }
            }
          }
          break;
        case HAM_PARAM_RECORD_SIZE:
          rec_size = (ham_u32_t)param->value;
          break;
        default:
          ham_trace(("invalid parameter 0x%x (%d)", param->name, param->name));
          return (HAM_INV_PARAMETER);
      }
    }
  }

  if (key_type == HAM_TYPE_UINT8
        || key_type == HAM_TYPE_UINT16
        || key_type == HAM_TYPE_UINT32
        || key_type == HAM_TYPE_REAL32
        || key_type == HAM_TYPE_REAL64) {
    if (flags & HAM_RECORD_NUMBER) {
      ham_trace(("HAM_RECORD_NUMBER not allowed in combination with "
                      "fixed length type"));
      return (HAM_INV_PARAMETER);
    }
  }
  if (flags & HAM_RECORD_NUMBER)
    key_type = HAM_TYPE_UINT64;

  // Pro: Key compression is not allowed for PAX layouts!
  if (key_compressor != HAM_COMPRESSOR_NONE) {
    if (key_type != HAM_TYPE_BINARY || key_size != HAM_KEY_SIZE_UNLIMITED) {
      ham_trace(("Key compression only allowed for unlimited binary keys "
                 "(HAM_TYPE_BINARY"));
      return (HAM_INV_PARAMETER);
    }
  }

  ham_u32_t mask = HAM_FORCE_RECORDS_INLINE
                    | HAM_FLUSH_WHEN_COMMITTED
                    | HAM_ENABLE_DUPLICATE_KEYS
                    | HAM_RECORD_NUMBER;
  if (flags & ~mask) {
    ham_trace(("invalid flags(s) 0x%x", flags & ~mask));
    return (HAM_INV_PARAMETER);
  }

  /* check if this database name is unique */
  ham_assert(m_header->get_max_databases() > 0);
  for (ham_u32_t i = 0; i < m_header->get_max_databases(); i++) {
    ham_u16_t name = get_btree_descriptor(i)->get_dbname();
    if (!name)
      continue;
    if (name == dbname) {
      return (HAM_DATABASE_ALREADY_EXISTS);
    }
  }

  /* find a free slot in the PBtreeHeader array and store the name */
  ham_assert(m_header->get_max_databases() > 0);
  for (dbi = 0; dbi < m_header->get_max_databases(); dbi++) {
    ham_u16_t name = get_btree_descriptor(dbi)->get_dbname();
    if (!name) {
      get_btree_descriptor(dbi)->set_dbname(dbname);
      break;
    }
  }
  if (dbi == m_header->get_max_databases())
    return (HAM_LIMITS_REACHED);

  /* logging enabled? then the changeset HAS to be empty */
#ifdef HAM_DEBUG
  if (get_flags() & HAM_ENABLE_RECOVERY)
    ham_assert(get_changeset().is_empty());
#endif

  /* create a new Database object */
  LocalDatabase *db = new LocalDatabase(this, dbname, flags);

  /* initialize the Database */
  ham_status_t st = db->create(dbi, key_type, key_size, rec_size);
  if (st) {
    delete db;
    return (st);
  }

  /* enable compression? */
  if (record_compressor)
    db->enable_record_compression(record_compressor);
  if (key_compressor)
    db->enable_key_compression(key_compressor);

  mark_header_page_dirty();

  /* if logging is enabled: flush the changeset and the header page */
  if (st == 0 && get_flags() & HAM_ENABLE_RECOVERY)
    get_changeset().flush(get_incremented_lsn());

  /*
   * on success: store the open database in the environment's list of
   * opened databases
   */
  get_database_map()[dbname] = db;

  *pdb = db;
  
  return (0);
}

ham_status_t
LocalEnvironment::open_db(Database **pdb, ham_u16_t dbname,
                ham_u32_t flags, const ham_parameter_t *param)
{
  ham_u16_t dbi;
  ham_u32_t record_compressor = HAM_COMPRESSOR_NONE;

  *pdb = 0;

  ham_u32_t mask = HAM_FORCE_RECORDS_INLINE
                    | HAM_FLUSH_WHEN_COMMITTED
                    | HAM_READ_ONLY;
  if (flags & ~mask) {
    ham_trace(("invalid flags(s) 0x%x", flags & ~mask));
    return (HAM_INV_PARAMETER);
  }

  if (param) {
    for (; param->name; param++) {
      switch (param->name) {
        case HAM_PARAM_RECORD_COMPRESSION:
          ham_trace(("Record compression parameters are only allowed in "
                     "ham_env_create_db"));
          return (HAM_INV_PARAMETER);
        case HAM_PARAM_KEY_COMPRESSION:
          ham_trace(("Key compression parameters are only allowed in "
                     "ham_env_create_db"));
          return (HAM_INV_PARAMETER);
        default:
          ham_trace(("invalid parameter 0x%x (%d)", param->name, param->name));
          return (HAM_INV_PARAMETER);
      }
    }
  }

  /* make sure that this database is not yet open */
  if (get_database_map().find(dbname) != get_database_map().end())
    return (HAM_DATABASE_ALREADY_OPEN);

  ham_assert(get_device());
  ham_assert(0 != m_header->get_header_page());
  ham_assert(m_header->get_max_databases() > 0);

  /* search for a database with this name */
  for (dbi = 0; dbi < m_header->get_max_databases(); dbi++) {
    ham_u16_t name = get_btree_descriptor(dbi)->get_dbname();
    if (!name)
      continue;
    if (dbname == name)
      break;
  }

  if (dbi == m_header->get_max_databases())
    return (HAM_DATABASE_NOT_FOUND);

  /* create a new Database object */
  LocalDatabase *db = new LocalDatabase(this, dbname, flags);

  /* open the database */
  ham_status_t st = db->open(dbi);
  if (st) {
    delete db;
    ham_trace(("Database could not be opened"));
    return (st);
  }

  /* enable compression? */
  if (record_compressor)
    db->enable_record_compression(record_compressor);

  /*
   * on success: store the open database in the environment's list of
   * opened databases
   */
  get_database_map()[dbname] = db;

  *pdb = db;

  return (0);
}

Transaction *
LocalEnvironment::txn_begin(const char *name, ham_u32_t flags)
{
  return (m_txn_manager->begin(name, flags));
}

void
LocalEnvironment::recover(ham_u32_t flags)
{
  ham_status_t st = 0;
  m_journal = new Journal(this);

  ham_assert(get_flags() & HAM_ENABLE_RECOVERY);

  try {
    m_journal->open();
  }
  catch (Exception &ex) {
    if (ex.code == HAM_FILE_NOT_FOUND)
      m_journal->create();
  }

  /* success - check if we need recovery */
  if (!m_journal->is_empty()) {
    if (flags & HAM_AUTO_RECOVERY) {
      m_journal->recover();
    }
    else {
      st = HAM_NEED_RECOVERY;
      goto bail;
    }
  }

bail:
  /* in case of errors: close log and journal, but do not delete the files */
  if (st) {
    m_journal->close(true);
    delete m_journal;
    m_journal = 0;
    throw Exception(st);
  }

  /* done with recovering - if there's no log and/or no journal then
   * create them and store them in the environment */
  if (!(get_flags() & HAM_ENABLE_TRANSACTIONS)) {
    delete m_journal;
    m_journal = 0;
  }

  /* reset the page manager */
  m_page_manager->close();
}

void
LocalEnvironment::get_metrics(ham_env_metrics_t *metrics) const
{
  // PageManager metrics (incl. cache and freelist)
  m_page_manager->get_metrics(metrics);
  // the BlobManagers
  m_blob_manager->get_metrics(metrics);
  // the Journal (if available)
  if (m_journal)
    m_journal->get_metrics(metrics);
  // and of the btrees
  BtreeIndex::get_metrics(metrics);
  // SIMD support enabled?
  metrics->simd_lane_width = os_get_simd_lane_width();
}

ham_u64_t
LocalEnvironment::get_incremented_lsn()
{
  Journal *j = get_journal();
  if (j)
    return (j->get_incremented_lsn());
  LocalTransactionManager *ltm = (LocalTransactionManager *)m_txn_manager;
  return (ltm->get_incremented_lsn());
}

} // namespace hamsterdb
