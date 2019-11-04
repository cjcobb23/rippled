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

void populateTransaction(
        io::xpring::Transaction* txn_proto,
        std::shared_ptr<STTx const> txn_st)
{
    STAmount fee = txn_st->getFieldAmount(sfFee);
    txn_proto->mutable_fee()->set_drops(std::to_string(fee.xrp().drops()));
    //TODO: what about when the amount is not xrp?
    //What does the data look like then
    STAmount amount = txn_st->getFieldAmount(sfAmount);
    txn_proto->mutable_payment()->mutable_xrp_amount()->set_drops(std::to_string(amount.xrp().drops()));


    //TODO return account as bytes or base58 string?
    AccountID account = txn_st->getAccountID(sfAccount);
    const char* account_data = reinterpret_cast<const char*>(account.data());

    std::string account_str{account_data,account.size()};

    //std::cout << "account is " << toBase58(account) << std::endl;

    txn_proto->set_account(account_str);

    AccountID account_dest = txn_st->getAccountID(sfDestination);
    const char* account_dest_data = reinterpret_cast<const char*>(account_dest.data());


    std::string account_dest_str{account_dest_data,account_dest.size()};
    txn_proto->mutable_payment()->set_destination(account_dest_str);
    txn_proto->set_sequence(txn_st->getFieldU32(sfSequence));
    Blob signingPubKey = txn_st->getFieldVL(sfSigningPubKey);
    std::string pubKeyStr{reinterpret_cast<const char*>(signingPubKey.data()),signingPubKey.size()};
    txn_proto->set_signing_public_key_hex(pubKeyStr);
}

std::pair<io::xpring::TxResponse, grpc::Status>
doTxGrpc(RPC::ContextGeneric<io::xpring::TxRequest>& context)
{
    io::xpring::TxResponse result;
    grpc::Status status = grpc::Status::OK;

    io::xpring::TxRequest& request = context.params;

    std::string const& hash_bytes = request.hash();

    uint256 hash = uint256::fromVoid(hash_bytes.data());
    std::cout << "hash is " << to_string(hash) << std::endl;

    std::shared_ptr<Transaction> txn = context.app.getMasterTransaction().fetch(hash, true);

    if(!txn)
    {
        std::cout << "txn not found" << std::endl;
        return {result,status};
        // return not found
    }
    std::shared_ptr<STTx const> st_txn = txn->getSTransaction();

/*    for(size_t i = 0; i < st_txn->getCount(); ++i)
    {
        SField const& field = st_txn->getFieldSType(i);
        std::cout << "field name is " << field.getName();
        
        std::cout << st_txn->peekAtField(field).getFullText() << std::endl;
    }
*/

    populateTransaction(result.mutable_tx(),st_txn);

    result.set_ledger_index(txn->getLedger());

    std::shared_ptr<Ledger const> ledger = 
        context.ledgerMaster.getLedgerBySeq(txn->getLedger());
    if(ledger)
    { 
        auto rawMeta = ledger->txRead (txn->getID()).second;
        if(rawMeta)
        {

            auto txMeta = std::make_shared<TxMeta>(
                    txn->getID(), ledger->seq(), *rawMeta);

            result.mutable_meta()->set_transaction_index(txMeta->getIndex());
            result.mutable_meta()->set_transaction_result(transToken(txMeta->getResultTER()));
            bool validated = isValidated(context.ledgerMaster,
                    ledger->info().seq,ledger->info().hash);
            std::cout << "validated is " << validated << std::endl;
            //TODO is this in the output when false?
            result.set_validated(validated);
        }
    }

    return {result, status};

}

} // ripple
