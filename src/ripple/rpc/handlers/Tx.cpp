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
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
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

static
bool
isValidated (RPC::Context& context, std::uint32_t seq, uint256 const& hash)
{
    return isValidated(context.ledgerMaster, seq, hash);
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

Json::Value doTx (RPC::Context& context)
{
    if (!context.params.isMember (jss::transaction))
        return rpcError (rpcINVALID_PARAMS);

    bool binary = context.params.isMember (jss::binary)
            && context.params[jss::binary].asBool ();

    auto const txid  = context.params[jss::transaction].asString ();

    if (!isHexTxID (txid))
        return rpcError (rpcNOT_IMPL);

    auto txn = context.app.getMasterTransaction ().fetch (
        from_hex_text<uint256>(txid), true);

    if (!txn)
        return rpcError (rpcTXN_NOT_FOUND);

    Json::Value ret = txn->getJson (JsonOptions::include_date, binary);

    if (txn->getLedger () == 0)
        return ret;

    if (auto lgr = context.ledgerMaster.getLedgerBySeq (txn->getLedger ()))
    {
        bool okay = false;

        if (binary)
        {
            std::string meta;

            if (getMetaHex (*lgr, txn->getID (), meta))
            {
                ret[jss::meta] = meta;
                okay = true;
            }
        }
        else
        {
            auto rawMeta = lgr->txRead (txn->getID()).second;
            if (rawMeta)
            {
                auto txMeta = std::make_shared<TxMeta>(
                    txn->getID(), lgr->seq(), *rawMeta);
                okay = true;
                auto meta = txMeta->getJson (JsonOptions::none);
                insertDeliveredAmount (meta, context, txn, *txMeta);
                ret[jss::meta] = std::move(meta);
            }
        }

        if (okay)
            ret[jss::validated] = isValidated (
                context, lgr->info().seq, lgr->info().hash);
    }

    return ret;
}

template <class T>
void populateFields(T& proto,STObject const& obj, std::uint16_t type)
{
    if(type == ltACCOUNT_ROOT)
    {
        RPC::populateAccountRoot(*proto.mutable_account_root(),obj);
    }
    else if(type == ltRIPPLE_STATE)
    {
        RPC::populateRippleState(*proto.mutable_ripple_state(),obj);
    }
    else if(type == ltOFFER)
    {
        RPC::populateOffer(*proto.mutable_offer(),obj);
    }
    else if(type == ltDIR_NODE)
    {
        RPC::populateDirectoryNode(*proto.mutable_directory_node(),obj);
    }
    else
    {
        //Ledger object not supported
    }
}

std::string txnTypeString(TxType type)
{
    return TxFormats::getInstance().findByType(type)->getName();
}

std::pair<rpc::v1::TxResponse, grpc::Status>
doTxGrpc(RPC::ContextGeneric<rpc::v1::TxRequest>& context)
{
    //return values
    rpc::v1::TxResponse result;
    grpc::Status status = grpc::Status::OK;

    rpc::v1::TxRequest& request = context.params;

    std::string const& hash_bytes = request.hash();
    uint256 hash = uint256::fromVoid(hash_bytes.data());
    
    std::shared_ptr<Transaction> txn =
        context.app.getMasterTransaction().fetch(hash, true);
    if(!txn)
    {
        grpc::Status error_status{grpc::StatusCode::NOT_FOUND,"txn not found"};
        return {result,error_status};
    }

    std::shared_ptr<STTx const> st_txn = txn->getSTransaction();
    if(st_txn->getTxnType() != ttPAYMENT)
    {
        grpc::Status error_status{grpc::StatusCode::UNIMPLEMENTED,
            "txn type not supported: " + txnTypeString(st_txn->getTxnType())}; 
    }

    if(request.binary())
    {
        Serializer s = st_txn->getSerializer();
        result.set_tx_bytes(toBytes(s.peekData()));
    }
    else
    {
        RPC::populateTransaction(*result.mutable_tx(),st_txn);
    }

    result.set_ledger_index(txn->getLedger());
    std::shared_ptr<Ledger const> ledger = 
        context.ledgerMaster.getLedgerBySeq(txn->getLedger());
    if(ledger)
    { 
        if(request.binary())
        {
            SHAMapTreeNode::TNType type;
            auto const item =
                ledger->txMap().peekItem (txn->getID(), type);

            if (item && type == SHAMapTreeNode::tnTRANSACTION_MD)
            {
                SerialIter it (item->slice());
                it.getVL (); // skip transaction
                Slice slice = makeSlice(it.getVL ());
                result.set_meta_bytes(toBytes(slice));

                bool validated = isValidated(context.ledgerMaster,
                        ledger->info().seq,ledger->info().hash);
                result.set_validated(validated);
            }
        }
        else
        {
            auto rawMeta = ledger->txRead (txn->getID()).second;
            if(rawMeta)
            {
                auto txMeta = std::make_shared<TxMeta>(
                        txn->getID(), ledger->seq(), *rawMeta);

                bool validated = isValidated(context.ledgerMaster,
                        ledger->info().seq,ledger->info().hash);
                result.set_validated(validated);

                RPC::populateMeta(*result.mutable_meta(), txMeta);
            }
        }
    }
    return {result, status};
}

} // ripple
