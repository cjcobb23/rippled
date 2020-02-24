//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/GRPCHelpers.h>
#include <ripple/rpc/GRPCHandlers.h>

namespace ripple {

// {
//   transaction: <hex>
// }

static
bool
isHexTxID (std::string const& txid)
{
    if (txid.size () != 64)
        return false;

    auto const ret = std::find_if (txid.begin (), txid.end (),
        [](std::string::value_type c)
        {
            return !std::isxdigit (static_cast<unsigned char>(c));
        });

    return (ret == txid.end ());
}

static
bool
isValidated(LedgerMaster& ledgerMaster, std::uint32_t seq, uint256 const& hash)
{
    if (!ledgerMaster.haveLedger (seq))
        return false;

    if (seq > ledgerMaster.getValidatedLedger ()->info().seq)
        return false;

    return ledgerMaster.getHashBySeq (seq) == hash;
}

bool
getMetaHex (Ledger const& ledger,
    uint256 const& transID, std::string& hex)
{
    SHAMapTreeNode::TNType type;
    auto const item =
        ledger.txMap().peekItem (transID, type);

    if (!item)
        return false;

    if (type != SHAMapTreeNode::tnTRANSACTION_MD)
        return false;

    SerialIter it (item->slice());
    it.getVL (); // skip transaction
    hex = strHex (makeSlice(it.getVL ()));
    return true;
}

struct TxResult
{
    Transaction::pointer txn;

    std::variant<std::shared_ptr<TxMeta>, Blob> meta;

    bool validated = false;

    std::optional<bool> searchedAll;
};

struct TxArgs
{
    uint256 hash;

    bool binary = false;

    std::optional<std::pair<uint32_t,uint32_t>> ledgerRange;
};

std::pair<TxResult, RPC::Status>
doTxHelp(TxArgs& args, RPC::Context& context)
{
    TxResult result;

    ClosedInterval<uint32_t> range;

    if (args.ledgerRange)
    {
        constexpr uint16_t MAX_RANGE = 1000;

        if (args.ledgerRange->second < args.ledgerRange->first)
            return {result, rpcINVALID_LGR_RANGE};

        if (args.ledgerRange->second - args.ledgerRange->first > MAX_RANGE)
            return {result, rpcEXCESSIVE_LGR_RANGE};

        range = ClosedInterval<uint32_t>(
            args.ledgerRange->first, args.ledgerRange->second);
    }

    std::shared_ptr<Transaction> txn;
    auto ec{rpcSUCCESS};

    if (args.ledgerRange)
    {
        boost::variant<std::shared_ptr<Transaction>, bool> v =
            context.app.getMasterTransaction().fetch(args.hash, range, ec);

        if (v.which() == 1)
        {
            result.searchedAll = boost::get<bool>(v);
            return {result, rpcTXN_NOT_FOUND};
        }
        else
        {
            txn = boost::get<std::shared_ptr<Transaction>>(v);
        }
    }
    else
    {
        txn = context.app.getMasterTransaction().fetch(args.hash, ec);
    }

    if (ec == rpcDB_DESERIALIZATION)
    {
        return {result, ec};
    }
    if (!txn)
    {
        return {result, rpcTXN_NOT_FOUND};
    }

    // populate transaction data
    result.txn = txn;
    if (txn->getLedger() == 0)
    {
        return {result, rpcSUCCESS};
    }

    std::shared_ptr<Ledger const> ledger =
        context.ledgerMaster.getLedgerBySeq(txn->getLedger());
    // get meta data
    if (ledger)
    {
        bool ok = false;
        if (args.binary)
        {
            SHAMapTreeNode::TNType type;
            auto const item = ledger->txMap().peekItem(txn->getID(), type);

            if (item && type == SHAMapTreeNode::tnTRANSACTION_MD)
            {
                ok = true;
                SerialIter it(item->slice());
                it.skip(it.getVLDataLength());  // skip transaction
                Blob blob = it.getVL();
                result.meta = std::move(blob);
            }
        }
        else
        {
            auto rawMeta = ledger->txRead(txn->getID()).second;
            if (rawMeta)
            {
                ok = true;
                result.meta = std::make_shared<TxMeta>(
                    txn->getID(), ledger->seq(), *rawMeta);
            }
        }
        if (ok)
        {
            result.validated = isValidated(
                context.ledgerMaster, ledger->info().seq, ledger->info().hash);
        }
    }

    return {result, rpcSUCCESS};
}

void
populateResponse(
    std::pair<TxResult, RPC::Status>& res,
    TxArgs& args,
    std::function<void(RPC::Status&)> const& handleErr,
    std::function<void(RPC::Status&, bool)> const& handleErrSearchedAll,
    std::function<void(Transaction::pointer)> const& populateTxn,
    std::function<void(Transaction::pointer, std::shared_ptr<TxMeta>)> const&
        populateMeta,
    std::function<void(Blob&)> const& populateMetaBinary,
    std::function<void(bool)> const& populateValidated)
{
    // handle errors
    if (res.second.toErrorCode() != rpcSUCCESS)
    {
        if (res.second.toErrorCode() == rpcTXN_NOT_FOUND &&
            res.first.searchedAll.has_value())
        {
            handleErrSearchedAll(res.second, *res.first.searchedAll);
        }
        else
        {
            handleErr(res.second);
        }
    }
    // no errors
    else if (res.first.txn)
    {
        populateTxn(res.first.txn);

        // populate binary metadata
        if (args.binary && std::holds_alternative<Blob>(res.first.meta))
        {
            populateMetaBinary(std::get<Blob>(res.first.meta));
        }
        // populate meta data
        else if (std::holds_alternative<std::shared_ptr<TxMeta>>(
                     res.first.meta))
        {
            // check that meta is not nullptr
            if (auto& meta = std::get<std::shared_ptr<TxMeta>>(res.first.meta))
            {
                populateMeta(res.first.txn, meta);
            }
        }
        populateValidated(res.first.validated);
    }
}

Json::Value
doTxJson(RPC::JsonContext& context)
{
    // Deserialize and validate JSON arguments

    if (!context.params.isMember(jss::transaction))
        return rpcError(rpcINVALID_PARAMS);

    std::string txHash = context.params[jss::transaction].asString();
    if (!isHexTxID(txHash))
        return rpcError(rpcNOT_IMPL);

    TxArgs args;
    args.hash = from_hex_text<uint256>(txHash);

    args.binary = context.params.isMember(jss::binary) &&
        context.params[jss::binary].asBool();

    if (context.params.isMember(jss::min_ledger) &&
        context.params.isMember(jss::max_ledger))
    {
        try
        {
            args.ledgerRange = std::make_pair(
                context.params[jss::min_ledger].asUInt(),
                context.params[jss::max_ledger].asUInt());
        }
        catch (...)
        {
            // One of the calls to `asUInt ()` failed.
            return rpcError(rpcINVALID_LGR_RANGE);
        }
    }

    // Get data
    std::pair<TxResult, RPC::Status> res = doTxHelp(args, context);

    Json::Value ret;
    auto fillTxn = [&args, &ret](auto const& txn) {
        ret = txn->getJson(JsonOptions::include_date, args.binary);
    };

    auto handleErr = [&ret](auto const& error) {
        const_cast<RPC::Status&>(error).inject(ret);
    };

    auto handleErrSearchedAll = [&ret](
                             auto const& error, auto const& searchedAll) {
        ret = Json::Value(Json::objectValue);
        ret[jss::searched_all] = searchedAll;
        const_cast<RPC::Status&>(error).inject(ret);
    };

    auto fillMetaBinary = [&ret](auto const& metaBlob) {
        ret[jss::meta] = strHex(makeSlice(metaBlob));
    };

    auto fillMeta = [&ret, &context](auto const& txn, auto const& meta) {
        ret[jss::meta] = meta->getJson(JsonOptions::none);
        insertDeliveredAmount(ret[jss::meta], context, txn, *meta);
    };


    auto fillValidated = [&ret](auto const& validated) {
        ret[jss::validated] = validated;
    };

    populateResponse(
        res,
        args,
        handleErr,
        handleErrSearchedAll,
        fillTxn,
        fillMeta,
        fillMetaBinary,
        fillValidated);

    return ret;
}

std::pair<org::xrpl::rpc::v1::GetTransactionResponse, grpc::Status>
doTxGrpc(RPC::GRPCContext<org::xrpl::rpc::v1::GetTransactionRequest>& context)
{
    // return values
    org::xrpl::rpc::v1::GetTransactionResponse response;
    grpc::Status status = grpc::Status::OK;

    // input
    org::xrpl::rpc::v1::GetTransactionRequest& request = context.params;

    TxArgs args;

    std::string const& hashBytes = request.hash();
    args.hash = uint256::fromVoid(hashBytes.data());
    if (args.hash.size() != hashBytes.size())
    {
        grpc::Status errorStatus{grpc::StatusCode::INVALID_ARGUMENT,
                                 "ledger hash malformed"};
        return {response, errorStatus};
    }

    args.binary = request.binary();

    if (request.ledger_range().ledger_index_min() != 0 &&
        request.ledger_range().ledger_index_max() != 0)
    {
        args.ledgerRange = std::make_pair(
            request.ledger_range().ledger_index_min(),
            request.ledger_range().ledger_index_max());
    }

    std::pair<TxResult, RPC::Status> res = doTxHelp(args, context);

    auto handleErr = [&status](auto const& error) {
        if (error.toErrorCode() == rpcTXN_NOT_FOUND)
            status = {grpc::StatusCode::NOT_FOUND, "txn not found"};
        else
            status = {grpc::StatusCode::INTERNAL, error.message()};
    };

    auto handleErrSearchedAll = [&status](
                                    auto const&, auto const& searchedAll) {
        status = {grpc::StatusCode::NOT_FOUND,
                  "txn not found. searched_all = " + searchedAll};
    };

    auto fillTxn = [&context, &args, &request, &response](auto const& txn) {
        std::shared_ptr<STTx const> stTxn = txn->getSTransaction();
        if (args.binary)
        {
            Serializer s = stTxn->getSerializer();
            response.set_transaction_binary(s.data(), s.size());
        }
        else
        {
            RPC::populateTransaction(*response.mutable_transaction(), stTxn);
        }

        response.set_hash(std::move(request.hash()));

        auto ledgerIndex = txn->getLedger();
        response.set_ledger_index(ledgerIndex);
        if (ledgerIndex)
        {
            auto ct =
                context.app.getLedgerMaster().getCloseTimeBySeq(ledgerIndex);
            if (ct)
                response.mutable_date()->set_value(
                    ct->time_since_epoch().count());
        }

        RPC::populateTransactionResultType(
            *response.mutable_meta()->mutable_transaction_result(),
            txn->getResult());
        response.mutable_meta()->mutable_transaction_result()->set_result(
            transToken(txn->getResult()));
    };

    auto fillMeta = [&response, &context](auto const& txn, auto const& meta) {
        RPC::populateMeta(*response.mutable_meta(), meta);
        auto amt = getDeliveredAmount(
            context, txn->getSTransaction(), *meta, [&txn]() {
                return txn->getLedger();
            });
        if (amt)
        {
            RPC::populateProtoAmount(
                *amt, *response.mutable_meta()->mutable_delivered_amount());
        }
    };

    auto fillMetaBinary = [&response](auto const& metaBlob) {
        Slice slice = makeSlice(metaBlob);
        response.set_meta_binary(slice.data(), slice.size());
    };

    auto fillValidated = [&response](auto const& validated) {
        response.set_validated(validated);
    };

    populateResponse(
        res,
        args,
        handleErr,
        handleErrSearchedAll,
        fillTxn,
        fillMeta,
        fillMetaBinary,
        fillValidated);

    return {response, status};
}

}  // namespace ripple
