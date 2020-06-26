//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2020 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/reporting/DBHelpers.h>
#include <ripple/app/reporting/ReportingETL.h>

#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace ripple {

void
ReportingETL::startWriter(std::shared_ptr<Ledger>& ledger)
{
    writer_ = std::thread{[this, &ledger]() {
        std::shared_ptr<SLE> sle;
        size_t num = 0;
        // TODO: if this call blocks, flushDirty in the meantime
        while (not stopping_ and (sle = writeQueue_.pop()))
        {
            assert(sle);
            // TODO get rid of this conditional
            if (!ledger->exists(sle->key()))
                ledger->rawInsert(sle);

            if (flushInterval_ != 0 and (num % flushInterval_) == 0)
            {
                JLOG(journal_.debug())
                    << "Flushing! key = " << strHex(sle->key());
                ledger->stateMap().flushDirty(
                    hotACCOUNT_NODE, ledger->info().seq, true);
            }
            ++num;
        }
    }};
}

// Downloads ledger in full from network. Returns empty shared_ptr on error
// @param sequence of ledger to download
// @return the full ledger. All data has been written to the database (key-value
// and relational). Empty shared_ptr on error
std::shared_ptr<Ledger>
ReportingETL::loadInitialLedger(uint32_t startingSequence)
{
    // check that database is actually empty
    auto ledger = std::const_pointer_cast<Ledger>(
        app_.getLedgerMaster().getValidatedLedger());
    if(ledger)
    {
        JLOG(journal_.fatal()) << __func__ << " : "
            << "Database is not empty";
        assert(false);
        return {};
    }

    // fetch the ledger from the network. This function will not return until
    // either the fetch is successful, or the server is being shutdown. This only
    // fetches the ledger header and the transactions+metadata
    // TODO change the function to actually return the value, or an optional
    org::xrpl::rpc::v1::GetLedgerResponse response;
    if (not fetchLedger(startingSequence, response, false))
        return {};
    std::vector<TxMeta> metas;
    // add the transactions to the ledger
    ledger = updateLedger(response, ledger, metas, false);

    auto start = std::chrono::system_clock::now();

    // start a writer thread 
    startWriter(ledger);

    // download the full account state map. This function returns immediately
    // and the download occurs asynchronously
    loadBalancer_.loadInitialLedger(startingSequence, writeQueue_);
    // wait for the download to finish
    joinWriter();
    // TODO handle case when there is a network error (other side dies)
    // Shouldn't try to flush in that scenario
    // Retry? Or just die?
    if (not stopping_)
    {
        //TODO handle write conflict
        flushLedger(ledger);
        if (app_.config().usePostgresTx())
            writeToPostgres(ledger->info(), metas);
    }
    auto end = std::chrono::system_clock::now();
    JLOG(journal_.debug()) << "Time to download and store ledger = "
                           << ((end - start).count()) / 1000000000.0
                           << " nanoseconds";
    return ledger;
}

void
ReportingETL::joinWriter()
{
    std::shared_ptr<SLE> null;
    writeQueue_.push(null);
    writer_.join();
}

void
ReportingETL::flushLedger(std::shared_ptr<Ledger>& ledger)
{
    JLOG(journal_.debug()) << __func__ << " : "
                           << "Flushing ledger. " << toString(ledger->info());
    // These are recomputed in setImmutable
    auto& accountHash = ledger->info().accountHash;
    auto& txHash = ledger->info().txHash;
    auto& ledgerHash = ledger->info().hash;

    auto start = std::chrono::system_clock::now();

    ledger->setImmutable(app_.config(), false);

    auto numFlushed = ledger->stateMap().flushDirty(
        hotACCOUNT_NODE, ledger->info().seq, true);

    auto numTxFlushed = ledger->txMap().flushDirty(
        hotTRANSACTION_NODE, ledger->info().seq, true);

    {
        Serializer s(128);
        s.add32(HashPrefix::ledgerMaster);
        addRaw(ledger->info(), s);
        app_.getNodeStore().store(
            hotLEDGER,
            std::move(s.modData()),
            ledger->info().hash,
            ledger->info().seq,
            true);
    }

    app_.getNodeStore().sync();

    auto end = std::chrono::system_clock::now();

    JLOG(journal_.debug()) << __func__ << " : "
                           << "Flushed " << numFlushed
                           << " nodes to nodestore from stateMap";
    JLOG(journal_.debug()) << __func__ << " : "
                           << "Flushed " << numTxFlushed
                           << " nodes to nodestore from txMap";

    // TODO move these checks to another function?
    if (numFlushed == 0 && roundMetrics.objectCount != 0)
    {
        JLOG(journal_.fatal()) << __func__ << " : "
                               << "Failed to flush state map";
        assert(false);
    }
    if (numTxFlushed == 0 && roundMetrics.txnCount == 0)
    {
        JLOG(journal_.fatal()) << __func__ << " : "
                               << "Failed to flush tx map";
        assert(false);
    }

    roundMetrics.flushTime = ((end - start).count()) / 1000000000.0;

    // Make sure calculated hashes are correct
    if (ledger->stateMap().getHash().as_uint256() != accountHash)
    {
        JLOG(journal_.fatal())
            << __func__ << " : "
            << "State map hash does not match. "
            << "Expected hash = " << strHex(accountHash) << "Actual hash = "
            << strHex(ledger->stateMap().getHash().as_uint256());
        assert(false);
    }

    if (ledger->txMap().getHash().as_uint256() != txHash)
    {
        JLOG(journal_.fatal())
            << __func__ << " : "
            << "Tx map hash does not match. "
            << "Expected hash = " << strHex(txHash) << "Actual hash = "
            << strHex(ledger->txMap().getHash().as_uint256());
        assert(false);
    }

    if (ledger->info().hash != ledgerHash)
    {
        JLOG(journal_.fatal())
            << __func__ << " : "
            << "Ledger hash does not match. "
            << "Expected hash = " << strHex(ledgerHash)
            << "Actual hash = " << strHex(ledger->info().hash);
        assert(false);
    }

    JLOG(journal_.info()) << __func__ << " : "
                          << "Successfully flushed ledger! "
                          << toString(ledger->info());
}

void
ReportingETL::publishLedger(std::shared_ptr<Ledger>& ledger)
{
    app_.getOPs().pubLedger(ledger);

    lastPublish_ = std::chrono::system_clock::now();
}

bool
ReportingETL::publishLedger(uint32_t ledgerSequence, uint32_t maxAttempts)
{
    JLOG(journal_.info()) << __func__ << " : "
                          << "Attempting to publish ledger = "
                          << ledgerSequence;
    size_t numAttempts = 0;
    while (!stopping_)
    {
        auto ledger = app_.getLedgerMaster().getLedgerBySeq(ledgerSequence);

        if (!ledger)
        {
            JLOG(journal_.warn())
                << __func__ << " : "
                << "Trying to publish. Could not find ledger with sequence = "
                << ledgerSequence;
            // We try maxAttempts times to publish the ledger, waiting one
            // second in between each attempt.
            // If the ledger is not present in the database after maxAttempts,
            // we attempt to take over as the writer. If the takeover fails,
            // doContinuousETL will return, and this node will go back to
            // publishing.
            // If the node is in strict read only mode, we simply
            // skip publishing this ledger and return false indicating the
            // publish failed
            if (numAttempts >= maxAttempts)
            {
                JLOG(journal_.error()) << __func__ << " : "
                                       << "Failed to publish ledger after "
                                       << numAttempts << " attempts.";
                if (!readOnly_)
                {
                    JLOG(journal_.info()) << __func__ << " : "
                                          << "Attempting to become ETL writer";
                    return false;
                }
                else
                {
                    JLOG(journal_.debug())
                        << __func__ << " : "
                        << "In strict read-only mode. "
                        << "Skipping publishing this ledger. "
                        << "Beginning fast forward.";
                    return false;
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                ++numAttempts;
            }
            continue;
        }
        app_.getOPs().pubLedger(ledger);
        lastPublish_ = std::chrono::system_clock::now();
        JLOG(journal_.info())
            << __func__ << " : "
            << "Published ledger. " << toString(ledger->info());
        return true;
    }
    return false;
}

bool
ReportingETL::fetchLedger(
    uint32_t idx,
    org::xrpl::rpc::v1::GetLedgerResponse& out,
    bool getObjects)
{

    JLOG(journal_.debug()) << __func__ << " : "
                           << "Attempting to fetch ledger with sequence = "
                           << idx;

    auto res = loadBalancer_.fetchLedger(out, idx, getObjects);

    JLOG(journal_.trace()) << __func__ << " : "
                           << "GetLedger reply = " << out.DebugString();
    return res;
}

std::shared_ptr<Ledger>
ReportingETL::updateLedger(
    org::xrpl::rpc::v1::GetLedgerResponse& in,
    std::shared_ptr<Ledger>& parent,
    std::vector<TxMeta>& out,
    bool updateSkiplist)
{
    std::shared_ptr<Ledger> next;
    JLOG(journal_.info()) << __func__ << " : "
                          << "Beginning ledger update";
    auto start = std::chrono::system_clock::now();

    LedgerInfo lgrInfo = deserializeHeader(
        makeSlice(in.ledger_header()), true);

    JLOG(journal_.debug()) << __func__ << " : "
                           << "Deserialized ledger header. "
                           << toString(lgrInfo);

    if (!parent)
    {
        next = std::make_shared<Ledger>(lgrInfo, app_.config(), app_.getNodeFamily());
    }
    else
    {
        next = std::make_shared<Ledger>(*parent, NetClock::time_point{});
        next->setLedgerInfo(lgrInfo);
        assert(next->info().seq == parent->info().seq + 1);
    }

    next->stateMap().clearSynching();
    next->txMap().clearSynching();

    for (auto& txn : in.transactions_list().transactions())
    {
        auto& raw = txn.transaction_blob();

        // TODO can this be done faster? Move?
        SerialIter it{raw.data(), raw.size()};
        STTx sttx{it};

        auto txSerializer = std::make_shared<Serializer>(sttx.getSerializer());

        TxMeta txMeta{
            sttx.getTransactionID(), next->info().seq, txn.metadata_blob()};

        auto metaSerializer =
            std::make_shared<Serializer>(txMeta.getAsObject().getSerializer());

        JLOG(journal_.trace())
            << __func__ << " : "
            << "Inserting transaction = " << sttx.getTransactionID();
        next->rawTxInsert(
            sttx.getTransactionID(), txSerializer, metaSerializer);

        // TODO use emplace to avoid this copy
        out.push_back(txMeta);
    }

    JLOG(journal_.debug())
        << __func__ << " : "
        << "Inserted all transactions. Number of transactions  = "
        << in.transactions_list().transactions_size();

    for (auto& state : in.ledger_objects())
    {
        auto& index = state.index();
        auto& data = state.data();

        auto key = uint256::fromVoid(index.data());
        // indicates object was deleted
        if (data.size() == 0)
        {
            JLOG(journal_.trace()) << __func__ << " : "
                                   << "Erasing object = " << key;
            if (next->exists(key))
                next->rawErase(key);
        }
        else
        {
            // TODO maybe better way to construct the SLE?
            // Is there any type of move ctor? Maybe use Serializer?
            // Or maybe just use the move cto?
            SerialIter it{data.data(), data.size()};
            std::shared_ptr<SLE> sle = std::make_shared<SLE>(it, key);

            // TODO maybe remove this conditional
            if (next->exists(key))
            {
                JLOG(journal_.trace()) << __func__ << " : "
                                       << "Replacing object = " << key;
                next->rawReplace(sle);
            }
            else
            {
                JLOG(journal_.trace()) << __func__ << " : "
                                       << "Inserting object = " << key;
                next->rawInsert(sle);
            }
        }
    }
    JLOG(journal_.debug())
        << __func__ << " : "
        << "Inserted/modified/deleted all objects. Number of objects = "
        << in.ledger_objects_size();

    if (updateSkiplist)
        next->updateSkipList();

    // update metrics
    auto end = std::chrono::system_clock::now();

    roundMetrics.updateTime = ((end - start).count()) / 1000000000.0;
    roundMetrics.txnCount = in.transactions_list().transactions().size();
    roundMetrics.objectCount = in.ledger_objects().size();

    JLOG(journal_.debug()) << __func__ << " : "
                           << "Finished ledger update";
    return next;
}

bool
ReportingETL::writeToPostgres(
    LedgerInfo const& info,
    std::vector<TxMeta>& metas)
{
    // TODO: clean this up a bit. use less auto, better error handling, etc
    JLOG(journal_.debug()) << __func__ << " : "
                           << "Beginning write to Postgres";
    if (!app_.pgPool())
    {
        JLOG(journal_.fatal()) << __func__ << " : "
                               << "app_.pgPool is null";
        assert(false);
    }
    std::shared_ptr<PgQuery> pg = std::make_shared<PgQuery>(app_.pgPool());
    std::shared_ptr<Pg> conn;

    auto start = std::chrono::system_clock::now();

    executeUntilSuccess(pg, conn, "BEGIN", PGRES_COMMAND_OK, *this);

    // Writing to the ledgers db fails if the ledger already exists in the db.
    // In this situation, the ETL process has detected there is another writer,
    // and falls back to only publishing
    if (!writeToLedgersDB(info, pg, conn, *this))
    {
        JLOG(journal_.warn()) << __func__ << " : "
                              << "Failed to write to ledgers database.";
        return false;
    }

    writeToAccountTransactionsDB(metas, pg, conn, *this);

    executeUntilSuccess(pg, conn, "COMMIT", PGRES_COMMAND_OK, *this);

    PQsetnonblocking(conn->getConn(), 1);
    app_.pgPool()->checkin(conn);

    auto end = std::chrono::system_clock::now();

    roundMetrics.postgresTime = ((end - start).count()) / 1000000000.0;
    JLOG(journal_.info()) << __func__ << " : "
                          << "Successfully wrote to Postgres";
    return true;
}

void
ReportingETL::outputMetrics(std::shared_ptr<Ledger>& ledger)
{
    roundMetrics.printMetrics(journal_, ledger->info());

    totalMetrics.addMetrics(roundMetrics);
    totalMetrics.printMetrics(journal_);

    // reset round metrics
    roundMetrics = {};
}

// Database must be populated when this starts
std::optional<uint32_t>
ReportingETL::runETLPipeline(uint32_t startSequence)
{
    JLOG(journal_.debug()) << __func__ << " : "
                           << "Starting etl pipeline";
    writing_ = true;

    std::shared_ptr<Ledger> parent = std::const_pointer_cast<Ledger>(
            app_.getLedgerMaster().getLedgerBySeq(startSequence-1));
    assert(parent);

    std::atomic_bool writeConflict = false;
    std::optional<uint32_t> lastPublishedSequence;
    constexpr uint32_t maxQueueSize = 1000;

    ThreadSafeQueue<std::optional<org::xrpl::rpc::v1::GetLedgerResponse>>
        transformQueue{maxQueueSize};

    std::thread extracter{
        [this, &startSequence, &writeConflict, &transformQueue]() {
            uint32_t currentSequence = startSequence;

            // there are two stopping conditions here.
            // First, if there is a write conflict in the load thread, the ETL
            // mechanism should stop.
            // Second, if the entire server is shutting down,
            // waitUntilValidatedByNetwork() is going to return false.
            while (networkValidatedLedgers_.waitUntilValidatedByNetwork(
                       currentSequence) &&
                   !writeConflict)
            {
                org::xrpl::rpc::v1::GetLedgerResponse fetchResponse;
                // if the fetch is unsuccessful, stop. fetchLedger only returns
                // false if the server is shutting down, or if the ledger was
                // found in the database. otherwise, fetchLedger will continue
                // trying to fetch the specified ledger until successful
                if (!fetchLedger(currentSequence, fetchResponse))
                {
                    break;
                }

                transformQueue.push(fetchResponse);
                ++currentSequence;
            }
            // empty optional tells the transformer to shut down
            transformQueue.push({});
        }};

    ThreadSafeQueue<
        std::optional<std::pair<std::shared_ptr<Ledger>, std::vector<TxMeta>>>>
        loadQueue{maxQueueSize};
    std::thread transformer{[this,
                             &parent,
                             &writeConflict,
                             &loadQueue,
                             &transformQueue]() {
        auto& next = parent;
        while (!writeConflict)
        {
            std::optional<org::xrpl::rpc::v1::GetLedgerResponse> fetchResponse =
                transformQueue.pop();
            // if fetchResponse is an empty optional, the extracter thread has
            // stopped and the transformer should stop as well
            if (!fetchResponse)
            {
                break;
            }

            std::vector<TxMeta> metas;
            next = updateLedger(*fetchResponse, next, metas);
            loadQueue.push(std::make_pair(next, metas));
        }
        // empty optional tells the loader to shutdown
        loadQueue.push({});
    }};

    std::thread loader{
        [this, &lastPublishedSequence, &loadQueue, &writeConflict]() {
            while (!writeConflict)
            {
                std::optional<
                    std::pair<std::shared_ptr<Ledger>, std::vector<TxMeta>>>
                    result = loadQueue.pop();
                // if result is an empty optional, the transformer thread has
                // stopped and the loader should stop as well
                if (!result)
                    break;

                auto& ledger = result->first;
                auto& metas = result->second;

                // write to the key-value store
                flushLedger(ledger);

                // write to RDBMS
                // if there is a write conflict, some other process has already
                // written this ledger and has taken over as the ETL writer
                if (app_.config().usePostgresTx())
                    if (!writeToPostgres(ledger->info(), metas))
                        writeConflict = true;

                // still publish even if we are relinquishing ETL control
                publishLedger(ledger);
                lastPublishedSequence = ledger->info().seq;
                checkConsistency(*this);
            }
        }};

    // wait for all of the threads to stop
    loader.join();
    extracter.join();
    transformer.join();
    writing_ = false;

    JLOG(journal_.debug()) << __func__ << " : "
                           << "Stopping etl pipeline";

    return lastPublishedSequence;
}

// main loop. The software begins monitoring the ledgers that are validated
// by the nework. The member networkValidatedLedgers_ keeps track of the
// sequences of ledgers validated by the network. Whenever a ledger is validated
// by the network, the software looks for that ledger in the database. Once the
// ledger is found in the database, the software publishes that ledger to the
// ledgers stream. If a network validated ledger is not found in the database
// after a certain amount of time, then the software attempts to take over
// responsibility of the ETL process, where it writes new ledgers to the
// database. The software will relinquish control of the ETL process if it
// detects that another process has taken over ETL.
void
ReportingETL::monitor()
{
    auto ledger = std::const_pointer_cast<Ledger>(
        app_.getLedgerMaster().getValidatedLedger());
    if (!ledger)
    {
        JLOG(journal_.info()) << __func__ << " : "
                              << "Database is empty. Will download a ledger "
                                 "from the network.";
        if (startSequence_)
        {
            JLOG(journal_.info())
                << __func__ << " : "
                << "ledger sequence specified in config. "
                << "Will begin ETL process starting with ledger "
                << *startSequence_;
            ledger = loadInitialLedger(*startSequence_);
        }
        else
        {
            JLOG(journal_.info())
                << __func__ << " : "
                << "Waiting for next ledger to be validated by network...";
            std::optional<uint32_t> mostRecentValidated =
                networkValidatedLedgers_.getMostRecent();
            if (mostRecentValidated)
            {
                JLOG(journal_.info()) << __func__ << " : "
                                      << "Ledger " << *mostRecentValidated
                                      << " has been validated. "
                                      << "Downloading...";
                ledger = loadInitialLedger(*mostRecentValidated);
            }
            else
            {
                JLOG(journal_.info()) << __func__ << " : "
                                      << "The wait for the next validated "
                                      << "ledger has been aborted. "
                                      << "Exiting monitor loop";
                return;
            }
        }
    }
    else
    {
        JLOG(journal_.info()) << __func__ << " : "
            << "Database already populated. Picking up from the tip of history";
    }
    if(!ledger)
    {
        JLOG(journal_.error()) << __func__ << " : "
            << "Failed to load initial ledger. Exiting monitor loop";
        return;
    }
    else
    {
        publishLedger(ledger);
    }
    uint32_t nextSequence = ledger->info().seq + 1;

    JLOG(journal_.debug()) << __func__ << " : "
                           << "Database is populated. "
                           << "Starting monitor loop. sequence = "
                           << nextSequence;
    while (
        !stopping_ &&
        networkValidatedLedgers_.waitUntilValidatedByNetwork(nextSequence))
    {
        JLOG(journal_.info()) << __func__ << " : "
                              << "Ledger with sequence = " << nextSequence
                              << " has been validated by the network. "
                              << "Attempting to find in database and publish";
        // Attempt to take over responsibility of ETL writer after 10 failed
        // attempts to publish the ledger. publishLedger() fails if the
        // ledger that has been validated by the network is not found in the
        // database after the specified number of attempts. publishLedger()
        // waits one second between each attempt to read the ledger from the
        // database
        //
        // In strict read-only mode, when the software fails to find a
        // ledger in the database that has been validated by the network,
        // the software will only try to publish subsequent ledgers once,
        // until one of those ledgers is found in the database. Once the
        // software successfully publishes a ledger, the software will fall
        // back to the normal behavior of trying several times to publish
        // the ledger that has been validated by the network. In this
        // manner, a reporting processing running in read-only mode does not
        // need to restart if the database is wiped.
        constexpr size_t timeoutSeconds = 10;
        bool success =
            publishLedger(nextSequence, timeoutSeconds);
        if (!success)
        {
            JLOG(journal_.warn()) << __func__ << " : "
                                  << "Failed to publish ledger with sequence = "
                                  << nextSequence << " . Beginning ETL";
            // doContinousETLPipelined returns the most recent sequence
            // published empty optional if no sequence was published
            std::optional<uint32_t> lastPublished =
                runETLPipeline(nextSequence);
            JLOG(journal_.info()) << __func__ << " : "
                                  << "Aborting ETL. Falling back to publishing";
            // if no ledger was published, don't increment nextSequence
            if (lastPublished)
                nextSequence = *lastPublished + 1;
        }
        else
        {
            ++nextSequence;
        }
    }
}

void
ReportingETL::monitorReadOnly()
{
    std::optional<uint32_t> mostRecent =
        networkValidatedLedgers_.getMostRecent();
    if (!mostRecent)
        return;
    uint32_t sequence = *mostRecent;
    bool success = true;
    while (!stopping_ &&
           networkValidatedLedgers_.waitUntilValidatedByNetwork(sequence))
    {
        success = publishLedger(sequence, success ? 30 : 1);
        ++sequence;
    }
}

void
ReportingETL::doWork()
{
    worker_ = std::thread([this]() {
        if (readOnly_)
            monitorReadOnly();
        else
            monitor();
    });
}

void
ReportingETL::setup()
{
    if (app_.config().START_UP == Config::StartUpType::FRESH && !readOnly_)
    {
        assert(app_.config().exists("reporting"));
        Section section = app_.config().section("reporting");
        std::pair<std::string, bool> startIndexPair =
            section.find("start_index");

        if (startIndexPair.second)
        {
            startSequence_ = std::stoi(startIndexPair.first);
        }
    }
    else if (!readOnly_)
    {
        if (checkConsistency_)
            assert(checkConsistency(*this));
    }
}

ReportingETL::ReportingETL(Application& app, Stoppable& parent)
    : Stoppable("ReportingETL", parent)
    , app_(app)
    , journal_(app.journal("ReportingETL"))
    , loadBalancer_(*this)
{
    // if present, get endpoint from config
    if (app_.config().exists("reporting"))
    {
        Section section = app_.config().section("reporting");

        JLOG(journal_.debug()) << "Parsing config info";

        std::pair<std::string, bool> ro = section.find("read_only");
        if (ro.second)
        {
            readOnly_ = ro.first == "true";
            app_.config().setReportingReadOnly(readOnly_);
        }

        auto& vals = section.values();
        for (auto& v : vals)
        {
            JLOG(journal_.debug()) << "val is " << v;
            Section source = app_.config().section(v);

            std::pair<std::string, bool> ipPair = source.find("source_ip");
            if (!ipPair.second)
                continue;

            std::pair<std::string, bool> wsPortPair =
                source.find("source_ws_port");
            if (!wsPortPair.second)
                continue;

            std::pair<std::string, bool> grpcPortPair =
                source.find("source_grpc_port");
            if (!grpcPortPair.second)
            {
                // add source without grpc port
                // used in read-only mode to detect when new ledgers have
                // been validated. Used for publishing
                if (app_.config().reportingReadOnly())
                    loadBalancer_.add(ipPair.first, wsPortPair.first);
                continue;
            }

            loadBalancer_.add(
                ipPair.first, wsPortPair.first, grpcPortPair.first);
        }

        std::pair<std::string, bool> pgTx = section.find("postgres_tx");
        if (pgTx.second)
            app_.config().setUsePostgresTx(pgTx.first == "true");

        // don't need to do any more work if we are in read only mode
        if (app_.config().reportingReadOnly())
            return;

        std::pair<std::string, bool> flushInterval =
            section.find("flush_interval");
        if (flushInterval.second)
        {
            flushInterval_ = std::stoi(flushInterval.first);
        }

        std::pair<std::string, bool> numMarkers = section.find("num_markers");
        if (numMarkers.second)
            numMarkers_ = std::stoi(numMarkers.first);

        std::pair<std::string, bool> checkConsistency =
            section.find("check_consistency");
        if (checkConsistency.second)
        {
            checkConsistency_ = (checkConsistency.first == "true");
        }

        if (checkConsistency_)
        {
            Section nodeDb = app_.config().section("node_db");

            std::pair<std::string, bool> postgresNodestore =
                nodeDb.find("type");
            // if the node_db is not using Postgres, we don't check for
            // consistency
            if (!postgresNodestore.second ||
                !boost::beast::iequals(postgresNodestore.first, "Postgres"))
                checkConsistency_ = false;
            // if we are not using postgres in place of SQLite, we don't
            // check for consistency
            if (!app_.config().usePostgresTx())
                checkConsistency_ = false;
        }
    }
}

}  // namespace ripple
