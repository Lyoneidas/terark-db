/**
 *    Copyright (C) 2016 Terark Inc.
 *    This file is heavily modified based on MongoDB WiredTiger StorageEngine
 *    Created on: 2015-12-01
 *    Author    : leipeng, rockeet@gmail.com
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#ifdef _WIN32
#define NVALGRIND
#endif
#ifdef _MSC_VER
#pragma warning(disable: 4800) // bool conversion
#pragma warning(disable: 4244) // 'return': conversion from '__int64' to 'double', possible loss of data
#pragma warning(disable: 4267) // '=': conversion from 'size_t' to 'int', possible loss of data
#endif

#include "terarkdb_kv_engine.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#if !defined(_MSC_VER)
#include <valgrind/valgrind.h>
#endif
#include "mongo/base/error_codes.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "terarkdb_customization_hooks.h"
#include "terarkdb_global_options.h"
#include "terarkdb_index.h"
#include "terarkdb_record_store.h"
#include "terarkdb_record_store_capped.h"
#include "terarkdb_recovery_unit.h"
#include "terarkdb_size_storer.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/background.h"
#include "mongo/util/exit.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include <terark/io/FileStream.hpp>

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo { namespace terarkdb {

namespace dps = ::mongo::dotted_path_support;

using std::set;
using std::string;
using terark::FileStream;

TableThreadData::TableThreadData(DbTable* tab) {
	m_dbCtx.reset(tab->createDbContext());
	m_dbCtx->syncIndex = false;
}

ThreadSafeTable::ThreadSafeTable(const fs::path& dbPath) {
	m_tab = DbTable::open(dbPath);
	m_indexForwardIterCache.resize(m_tab->getIndexNum());
	m_indexBackwardIterCache.resize(m_tab->getIndexNum());
}

ThreadSafeTable::~ThreadSafeTable() {
	log() << BOOST_CURRENT_FUNCTION << ": tabDir: " << m_tab->getDir().string()
		<< ", refcnt = " << m_tab->get_refcount();
	destroy();
}

TableThreadDataPtr ThreadSafeTable::allocTableThreadData() {
	TableThreadDataPtr ret;
	std::unique_lock<std::mutex> lock(this->m_cursorCacheMutex);
	if (m_cursorCache.empty()) {
		lock.unlock();
		ret = new TableThreadData(this->m_tab.get());
	}
	else {
		ret = m_cursorCache.pop_val();
	}
	return ret;
}

void ThreadSafeTable::releaseTableThreadData(TableThreadDataPtr ttd) {
	std::unique_lock<std::mutex> lock(this->m_cursorCacheMutex);
	m_cursorCache.push_back(std::move(ttd));
}

IndexIterDataPtr ThreadSafeTable::allocIndexIter(size_t indexId, bool forward) {
	auto tab = m_tab.get();
	IndexIterDataPtr iter;
	assert(indexId < tab->getIndexNum());
	assert(m_indexForwardIterCache.size() == tab->getIndexNum());
	assert(m_indexBackwardIterCache.size() == tab->getIndexNum());
	{
		std::unique_lock<std::mutex> lock(m_cursorCacheMutex);
		if (forward) {
			if (m_indexForwardIterCache[indexId].empty()) {
				lock.unlock();
				iter = new IndexIterData();
				iter->m_cursor = tab->createIndexIterForward(indexId);
			} else {
				iter = m_indexForwardIterCache[indexId].pop_val();
			}
		}
		else {
			if (m_indexBackwardIterCache[indexId].empty()) {
				lock.unlock();
				iter = new IndexIterData();
				iter->m_cursor = tab->createIndexIterBackward(indexId);
			} else {
				iter = m_indexBackwardIterCache[indexId].pop_val();
			}
		}
	}
	return iter;
}

void ThreadSafeTable::releaseIndexIter(size_t indexId, bool forward, IndexIterDataPtr iter) {
	assert(indexId < m_indexForwardIterCache.size());
	assert(m_indexForwardIterCache.size() == m_indexBackwardIterCache.size());
	std::unique_lock<std::mutex> lock(m_cursorCacheMutex);
	if (forward) {
		m_indexForwardIterCache[indexId].push_back(std::move(iter));
	} else {
		m_indexBackwardIterCache[indexId].push_back(std::move(iter));
	}
}

// brain dead mongodb may not delete RecordStore and SortedDataInterface
// so, workaround mongodb, call destroy in cleanShutdown()
void ThreadSafeTable::destroy() {
	log() << BOOST_CURRENT_FUNCTION
		<< ": mongodb will leak RecordStore and SortedDataInterface, destory underlying objects now";
	m_indexForwardIterCache.clear();
	m_indexBackwardIterCache.clear();
	m_cursorCache.clear();
	m_ttd.clear();
	log() << BOOST_CURRENT_FUNCTION << ": m_tab->refcnt = " << m_tab->get_refcount()
		<< ", m_ttd.size = " << m_ttd.size();
	m_tab = nullptr;
}

TableThreadData& ThreadSafeTable::getMyThreadData() {
	TableThreadDataPtr& ttd = m_ttd.local();
	if (terark_unlikely(!ttd)) {
		DbTable* tab = m_tab.get();
		ttd = new TableThreadData(tab);
	}
	return *ttd;
}

boost::filesystem::path
TerarkDbKVEngine::getTableDir(StringData ns, StringData ident) const {
	boost::filesystem::path dir;
	if (m_identAsDir) {
		dir = ident.toString();
	}
	else {
		auto dotPos = std::find(ns.begin(), ns.end(), '.');
		if (ns.end() == dotPos) {
			dir = ns.toString();
		}
		else {
			dir  = string(ns.begin(), dotPos);
			dir /= string(dotPos + 1, ns.end());
		}
	}
	return dir;
}

TerarkDbKVEngine::TerarkDbKVEngine(const std::string& path,
							   const std::string& extraOpenOptions,
							   size_t cacheSizeGB,
							   bool durable,
							   bool repair)
    : _path(path),
      _durable(durable),
      _sizeStorerSyncTracker(getGlobalServiceContext()->getFastClockSource(), 100000, Milliseconds(60 * 1000))
{
	m_fuckKVCatalog = nullptr;
    boost::filesystem::path basePath = path;
	m_pathTerark = basePath / "terark";
	m_pathWt = basePath / "wt";
	m_pathTerarkTables = m_pathTerark / "tables";
	m_identAsDir = true;
	try {
		boost::filesystem::create_directories(m_pathWt);
	}
	catch (const std::exception&) {
		if ( ! ( boost::filesystem::exists(m_pathWt) &&
			     boost::filesystem::is_directory(m_pathWt) )
		   ) {
		    LOG(1) << BOOST_CURRENT_FUNCTION << ": \"" << m_pathWt.string() << "\" is not a directory";
			throw;
		}
	}
	m_wtEngine.reset(new WiredTigerKVEngine(
				kWiredTigerEngineName,
				m_pathWt.string(),
				getGlobalServiceContext()->getFastClockSource(),
				extraOpenOptions,
				cacheSizeGB,
				durable,
				false, // ephemeral
				repair,
				false));

    _previousCheckedDropsQueued = Date_t::now();
/*
    log() << "terarkdb_open : " << path;
    for (auto& tabDir : fs::directory_iterator(m_pathTerark / "tables")) {
    //	std::string strTabDir = tabDir.path().string();
    	// DbTablePtr tab = DbTable::open(strTabDir);
    //	std::string tabIdent = tabDir.path().filename().string();
    //	auto ib = m_tables.insert_i(tabIdent, nullptr);
    //	invariant(ib.second);
    }
*/
    {
        fs::path fpath = m_pathTerark / "size-store.hash_strmap";
		_sizeStorer.setFilePath(fpath.string());
        if (fs::exists(fpath)) {
			_sizeStorer.fillCache();
        }
    }
//	DbTable::setCompressionThreadsNum(4);
}

TerarkDbKVEngine::~TerarkDbKVEngine() {
    cleanShutdown();
}

void TerarkDbKVEngine::cleanShutdown() {
    log() << "TerarkDbKVEngine shutting down ...";
//  syncSizeInfo(true);
	std::lock_guard<std::mutex> lock(m_mutex);
	m_indices.clear();
	for (size_t i = 0; i < m_tables.end_i(); ++i) {
		if (m_tables.is_deleted(i))
			continue;
		const fstring    key = m_tables.key(i);
		ThreadSafeTable* tab = m_tables.val(i).get();
		log() << "table: " << key.str() << ", dir: " << tab->m_tab->getDir().string()
			<< ", ThreadSafeTable.refcnt = " << tab->get_refcount()
			<< ", DbTable.refcnt = " << tab->m_tab->get_refcount();

		// brain damaged mongodb leaks objects, so destroy it manually
		tab->destroy();
	}
    m_tables.clear();
	DbTable::safeStopAndWaitForFlush();
    log() << "TerarkDbKVEngine shutting down successed!";
//	std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void TerarkDbKVEngine::setJournalListener(JournalListener* jl) {
	m_wtEngine->setJournalListener(jl);
}

Status
TerarkDbKVEngine::okToRename(OperationContext* opCtx,
                           StringData fromNS,
                           StringData toNS,
                           StringData ident,
                           const RecordStore* originalRecordStore) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	size_t i = m_tables.find_i(ident);
	if (m_tables.end_i() == i) {
		return m_wtEngine->
			okToRename(opCtx, fromNS, toNS, ident, originalRecordStore);
	}
	if (m_identAsDir) {
		// do nothing...
		auto tabDir = m_pathTerarkTables / ident.toString() / "myname.txt";
		FileStream(tabDir.string(), "w").puts(toNS);
	}
	else {
	//	fs::rename(m_pathTerarkTables / getTableDir(fromNS),
	//			   m_pathTerarkTables / getTableDir(toNS));
		return Status(ErrorCodes::CommandNotSupported,
			"rename is not supported when using [namespace/collection-name] as dir");
	}
    return Status::OK();
}

int64_t
TerarkDbKVEngine::getIdentSize(OperationContext* opCtx, StringData ident) {
	std::lock_guard<std::mutex> lock(m_mutex);
	size_t i = m_tables.find_i(ident);
	if (m_tables.end_i() == i) {
		return m_wtEngine->getIdentSize(opCtx, ident);
	}
	ThreadSafeTable* tab = m_tables.val(i).get();
	return tab->m_tab->dataStorageSize();
}

Status
TerarkDbKVEngine::repairIdent(OperationContext* opCtx, StringData ident) {
	// ident must be a table
	invariant(ident.startsWith("collection-"));
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		size_t i = m_tables.find_i(ident);
		if (i < m_tables.end_i()) {
			return Status::OK();
		}
	}
	return m_wtEngine->repairIdent(opCtx, ident);
}

int TerarkDbKVEngine::flushAllFiles(bool sync) {
    LOG(1) << "TerarkDbKVEngine::flushAllFiles";
	terark::valvec<DbTablePtr>
		tabCopy(m_tables.end_i()+2, terark::valvec_reserve());
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_tables.for_each([&](const TableMap::value_type& x) {
			tabCopy.push_back(x.second->m_tab);
		});
	}
//  syncSizeInfo(true);
	for (auto& tabPtr : tabCopy) {
		tabPtr->flush();
	}
//  _sessionCache->waitUntilDurable(true);
	m_wtEngine->flushAllFiles(sync);
    return 1;
}

Status TerarkDbKVEngine::beginBackup(OperationContext* txn) {
    invariant(!_backupSession);
	m_wtEngine->beginBackup(txn);
    return Status::OK();
}

void TerarkDbKVEngine::endBackup(OperationContext* txn) {
    _backupSession.reset();
}

RecoveryUnit* TerarkDbKVEngine::newRecoveryUnit() {
	return m_wtEngine->newRecoveryUnit();
//    return new TerarkDbRecoveryUnit();
}

void TerarkDbKVEngine::setRecordStoreExtraOptions(const std::string& options) {
    _rsOptions = options;
}

void TerarkDbKVEngine::setSortedDataInterfaceExtraOptions(const std::string& options) {
    _indexOptions = options;
}

Status
TerarkDbKVEngine::createRecordStore(OperationContext* opCtx,
								  StringData ns,
                                  StringData ident,
                                  const CollectionOptions& options) {
	if (ident == "_mdb_catalog") {
		return m_wtEngine->createRecordStore(opCtx, ns, ident, options);
	}
	if (NamespaceString(ns).isOnInternalDb()) {
		return m_wtEngine->createRecordStore(opCtx, ns, ident, options);
	}
    if (options.capped) {
		// now don't use TerarkDbRecordStoreCapped
		// use TerarkDbRecordStoreCapped when we need hook some virtual functions
		return m_wtEngine->createRecordStore(opCtx, ns, ident, options);
    }
    LOG(2) << "TerarkDbKVEngine::createRecordStore: ns:" << ns << ", ident: " << ident
		<< "\noptions.storageEngine: " << options.storageEngine.jsonString(Strict, true)
		<< "\noptions.indexOptionDefaults: " << options.indexOptionDefaults.jsonString(Strict, true)
		;
	auto tabDir = m_pathTerarkTables / getTableDir(ns, ident);
    LOG(2)	<< "TerarkDbKVEngine::createRecordStore: ns:" << ns
			<< ", tabDir=" << tabDir.string();
	if (fs::exists(tabDir)) {
	//	return Status(ErrorCodes::FileAlreadyOpen, "Collection is already existed");
	}
	else {
		// TODO: parse options.storageEngine.TerarkSegDB to define schema
		// return Status(ErrorCodes::CommandNotSupported,
		//	"dynamic create RecordStore is not supported, schema is required");
		BSONElement dbmetaElem = options.storageEngine[kTerarkDbEngineName];
		std::string dbmetaData;
		if (!dbmetaElem || !dbmetaElem.Obj().getField("RowSchema")) {
			if (terark::getEnvBool("MongoTerarkDB_DynamicCreateCollection")) {
				LOG(1) << "TerarkDbKVEngine::createRecordStore: ns:" << ns
					<< ", tabDir=" << tabDir.string() << ", DynamicCreateCollection";
				dbmetaData =
//      "_id": { "type": "fixed", "length": 12 },
//  "TableIndex": [
//    { "fields": "_id", "unique": true }
//  ]
R"({
  "This is a dynamically created collection": true,
  "CheckMongoType": true,
  "RowSchema": {
    "columns": {
      "$$" : { "type": "carbin" }
    }
  },
  "LastField": "for stupid json comma"
})";
			}
			else {
				// damn! mongodb does not allowing createRecordStore fails
				// but we still must fail
				std::ostringstream oss;
				oss << "TerarkDbKVEngine::createRecordStore: TerarkSegDB does not support dynamic creating collections, "
					   "must calling terarkCreateColl(...) in mongo shell client before inserting data into collection: ns = "
					<< ns.toString() << ", " << "ident = " << ident.toString() << "";
				return Status(ErrorCodes::CommandNotSupported, oss.str());
			}
		}
		else {
			bool includeFieldName = false;
			bool pretty = true;
			dbmetaData = dbmetaElem.jsonString(Strict, includeFieldName, pretty);
		}
		fs::create_directories(tabDir);
		FileStream((tabDir/"dbmeta.json").string(), "w").puts(dbmetaData);
		FileStream((tabDir/"myname.txt" ).string(), "w").puts(ns);
	}
	return Status::OK();
}

ThreadSafeTable*
TerarkDbKVEngine::openTable(StringData ns, StringData ident) {
	auto tabDir = m_pathTerarkTables / getTableDir(ns, ident);
	if (!fs::exists(tabDir)) {
		return NULL;
	}
	if (!fs::exists(tabDir / "dbmeta.json")) {
		LOG(1) << "TerarkDbKVEngine::openTable(ns=" << ns << ", ident=" << ident
			<< "): tabDir = " << tabDir.string() << ", dbmeta.json not existed";
		return NULL;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	size_t fidx = m_tables.find_i(ident);
	ThreadSafeTable* tab = NULL;
	if (fidx < m_tables.end_i()) {
		tab = m_tables.val(fidx).get();
	} else {
		tab = new ThreadSafeTable(tabDir);
		m_tables.insert_i(ident, tab);
	}
	return tab;
}

RecordStore*
TerarkDbKVEngine::getRecordStore(OperationContext* opCtx,
							   StringData ns,
							   StringData ident,
							   const CollectionOptions& options) {
	if (ident == "_mdb_catalog") {
		return m_wtEngine->getRecordStore(opCtx, ns, ident, options);
	}
	if (NamespaceString(ns).isOnInternalDb()) {
		return m_wtEngine->getRecordStore(opCtx, ns, ident, options);
	}
    if (options.capped) {
		// now don't use TerarkDbRecordStoreCapped
		// use TerarkDbRecordStoreCapped when we need hook some virtual functions
		// const bool ephemeral = false;
		return m_wtEngine->getRecordStore(opCtx, ns, ident, options);
    }

	ThreadSafeTable* tab = openTable(ns, ident);
	if (NULL == tab) {
		return NULL;
	}
    return new TerarkDbRecordStore(opCtx, ns, ident, tab, NULL);
}

static std::string getIndexKeyPattern(const BSONObj& kp) {
	std::string strkp;
	BSONForEach(elem, kp) {
		strkp.append(elem.fieldName());
		strkp.append(",");
	}
	strkp.pop_back();
	return strkp;
}

Status
TerarkDbKVEngine::createSortedDataInterface(OperationContext* opCtx,
                                          StringData ident,
                                          const IndexDescriptor* desc) {
	if (desc->getCollection()->ns().isOnInternalDb()) {
		return m_wtEngine->createSortedDataInterface(opCtx, ident, desc);
	}
    if (desc->getCollection()->isCapped()) {
		// now don't use TerarkDbRecordStoreCapped
		// use TerarkDbRecordStoreCapped when we need hook some virtual functions
		// const bool ephemeral = false;
		return m_wtEngine->createSortedDataInterface(opCtx, ident, desc);
    }

	std::string collIndexOptions;
    const Collection* collection = desc->getCollection();
    // Treat 'collIndexOptions' as an empty string when the collection member of 'desc' is NULL in
    // order to allow for unit testing TerarkDbKVEngine::createSortedDataInterface().
    if (collection) {
        const CollectionCatalogEntry* cce = collection->getCatalogEntry();
        const CollectionOptions collOptions = cce->getCollectionOptions(opCtx);

        if (!collOptions.indexOptionDefaults["storageEngine"].eoo()) {
            BSONObj storageEngineOptions = collOptions.indexOptionDefaults["storageEngine"].Obj();
            collIndexOptions = 
                dps::extractElementAtPath(storageEngineOptions, kTerarkDbEngineName + ".configString")
                    .valuestrsafe();
        }
    }
	const string tableNS = desc->getCollection()->ns().toString();
	const string tableIdent = m_fuckKVCatalog->getCollectionIdent(tableNS);
    LOG(2)	<< "TerarkDbKVEngine::createSortedDataInterface ident: " << ident
			<< ", tableNS=" << tableNS << ", tableIdent=" << tableIdent
			<< " collIndexOptions: " << collIndexOptions;
	if (tableIdent == "_mdb_catalog") {
		return m_wtEngine->createSortedDataInterface(opCtx, ident, desc);
	}
	ThreadSafeTable* tst = openTable(tableNS, tableIdent);
	if (tst) {
		std::string strkp = getIndexKeyPattern(desc->keyPattern());
		DbTable* tab = tst->m_tab.get();
		size_t indexId = tab->getIndexId(strkp);
		LOG(2) << "TerarkDbKVEngine::createSortedDataInterface: "
			<< "strkp = (" << strkp << "), kp = " << desc->keyPattern().toString()
			<< ", indexId = " << indexId << ", indexNum = " << tab->getIndexNum();
		if (indexId < tab->getIndexNum()) {
			return Status::OK();
		}
		if (terark::getEnvBool("MongoTerarkDB_DynamicCreateIndex")) {
			// forward dynamic index to wiredtiger
			return m_wtEngine->createSortedDataInterface(opCtx, ident, desc);
		}
	}
	return Status(ErrorCodes::CommandNotSupported,
					"dynamic creating index is not supported");
}

SortedDataInterface*
TerarkDbKVEngine::getSortedDataInterface(OperationContext* opCtx,
									   StringData ident,
									   const IndexDescriptor* desc)
{
	if (desc->getCollection()->ns().isOnInternalDb()) {
		return m_wtEngine->getSortedDataInterface(opCtx, ident, desc);
	}
    if (desc->getCollection()->isCapped()) {
		// now don't use TerarkDbRecordStoreCapped
		// use TerarkDbRecordStoreCapped when we need hook some virtual functions
		// const bool ephemeral = false;
		return m_wtEngine->getSortedDataInterface(opCtx, ident, desc);
    }
	const string tableNS = desc->getCollection()->ns().toString();
	const string tableIdent = m_fuckKVCatalog->getCollectionIdent(tableNS);
	if (tableIdent == "_mdb_catalog") {
		return m_wtEngine->getSortedDataInterface(opCtx, ident, desc);
	}
	ThreadSafeTable* tst = openTable(tableNS, tableIdent);
	if (!tst) {
		return NULL;
	}
	std::string strkp = getIndexKeyPattern(desc->keyPattern());
	DbTable* tab = tst->m_tab.get();
	size_t indexId = tab->getIndexId(strkp);
	if (indexId < tab->getIndexNum()) {
		if (desc->unique())
			return new TerarkDbIndexUnique(tst, opCtx, desc);
		else
    		return new TerarkDbIndexStandard(tst, opCtx, desc);
	}
	if (terark::getEnvBool("MongoTerarkDB_DynamicCreateIndex")) {
		// forward dynamic index to wiredtiger
		return m_wtEngine->getSortedDataInterface(opCtx, ident, desc);
	}
	return NULL;
}

Status TerarkDbKVEngine::dropIdent(OperationContext* opCtx, StringData ident) {
    LOG(1) << "TerarkDb dropIdent(): ident=" << ident << "\n";
#if 0
	LOG(1) << "TerarkDb dropIdent(): opCtx->getNS()=" << opCtx->getNS();
	bool isTerarkDb = false;
	const string tableIdent = m_fuckKVCatalog->getCollectionIdent(opCtx->getNS());
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		// Fuck, is opCtx->getNS() must be ident's NS?
		size_t i = m_tables.find_i(tableIdent);
		if (i < m_tables.end_i()) {
			if (ident == tableIdent) {
				ThreadSafeTablePtr& tabPtr = m_tables.val(i);
				tabPtr->m_tab->dropTable();
				m_tables.erase_i(i);
				isTerarkDb = true;
			}
			else {
				return Status(ErrorCodes::IllegalOperation,
								"TerarkDB don't support dropping index");
			}
		}
	}
	if (isTerarkDb) {
	//	table->dropTable() will remove directory
	//	fs::remove_all(m_pathTerarkTables / getTableDir(opCtx->getNS(), ident));
		return Status::OK();
	}
	return m_wtEngine->dropIdent(opCtx, ident);
#else
	// The fucking mongodb deleted opCtx->getNS()
	std::lock_guard<std::mutex> lock(m_mutex);
	size_t i = m_tables.find_i(ident);
	if (i < m_tables.end_i()) {
		ThreadSafeTablePtr& tabPtr = m_tables.val(i);
		tabPtr->m_tab->dropTable();
		LOG(1) << "tab->refcnt = " << tabPtr->m_tab->get_refcount();
		m_tables.erase_i(i);
	}
	else {
		Status s = m_wtEngine->dropIdent(opCtx, ident);
		if (!s.isOK()) {
			std::ostringstream oss;
			oss << "TerarkDB don't know wheater '" << ident << "' is an index or a collection, ";
			oss << "if it is, TerarkDB don't support deleting an index, if it is not, please fuck mongodb";
			return Status(ErrorCodes::IllegalOperation, oss.str());
		}
	}
	return Status::OK();
#endif
}

bool TerarkDbKVEngine::supportsDocLocking() const {
    return true;
}

bool TerarkDbKVEngine::supportsDirectoryPerDB() const {
    return !m_identAsDir;
}

bool TerarkDbKVEngine::hasIdent(OperationContext* opCtx, StringData ident) const {
    LOG(1) << "TerarkDbKVEngine::hasIdent(): ident=" << ident << "\n";
	if (ident.startsWith("index-")) {
		LOG(1) << "TerarkDbKVEngine::hasIdent(): brain damaged mongodb, "
				  "how can I simply get index with ident? ident the redundant damaged garbage concept";
	//	return false;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_tables.exists(ident))
			return true;
	}
	return m_wtEngine->hasIdent(opCtx, ident);
}

std::vector<std::string>
TerarkDbKVEngine::getAllIdents(OperationContext* opCtx) const {
#if 1
	return m_wtEngine->getAllIdents(opCtx);
#else
    std::vector<std::string> all = m_wtEngine->getAllIdents(opCtx);
    all.reserve(all.size() + m_tables.size());
	std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = m_tables.beg_i(); i < m_tables.end_i(); i = m_tables.next_i(i)) {
    	all.push_back(m_tables.key(i).str());
    }
    return all;
#endif
}

int TerarkDbKVEngine::reconfigure(const char* str) {
	return m_wtEngine->reconfigure(str);
}

} }  // namespace mongo::terark

