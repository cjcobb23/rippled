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

// @param[out] proto
// @param[in] txn_st
template <class T>
void populateAmount(T& proto, STAmount const& amount)
{
    if(amount.native())
    {
        proto.mutable_xrp_amount()->
            set_drops(amount.xrp().drops());
    }
    else
    {
        rpc::v1::FiatAmount* fiat =
            proto.mutable_fiat_amount();
        Issue const & issue = amount.issue();
        Currency currency = issue.currency;
        fiat->mutable_currency()->set_name(to_string(issue.currency));
        fiat->mutable_currency()->set_code(toBytes(currency));
        fiat->set_value(to_string(amount.iou()));
        fiat->set_issuer(toBase58(issue.account));
    }
}

// @param[out] txn_proto
// @param[in] txn_st
void populateTransaction(
        rpc::v1::Transaction& proto,
        std::shared_ptr<STTx const> txn_st)
{

    AccountID account = txn_st->getAccountID(sfAccount);
    proto.set_account(toBase58(account));

    STAmount amount = txn_st->getFieldAmount(sfAmount);
    populateAmount(*proto.mutable_payment()->mutable_amount(),amount);

    AccountID account_dest = txn_st->getAccountID(sfDestination);
    proto.mutable_payment()->set_destination(toBase58(account_dest));

    STAmount fee = txn_st->getFieldAmount(sfFee);
    proto.mutable_fee()->set_drops(fee.xrp().drops());

    proto.set_sequence(txn_st->getFieldU32(sfSequence));

    Blob signingPubKey = txn_st->getFieldVL(sfSigningPubKey);
    proto.set_signing_public_key_hex(toBytes(signingPubKey));

    proto.set_flags(txn_st->getFieldU32(sfFlags));

    proto.set_last_ledger_sequence(
            txn_st->getFieldU32(sfLastLedgerSequence));

    Blob blob = txn_st->getFieldVL(sfTxnSignature);
    proto.set_signature(toBytes(blob));

    if(txn_st->isFieldPresent(sfSendMax))
    {
        STAmount const & send_max = txn_st->getFieldAmount(sfSendMax);
        populateAmount(*proto.mutable_send_max(),send_max);
    }

    //populate path data
    STPathSet const & pathset = txn_st->getFieldPathSet(sfPaths);
    for(auto it = pathset.begin(); it < pathset.end(); ++it)
    {
        STPath const & path = *it;

        rpc::v1::Path* proto_path = proto.add_paths();

        for(auto it2 = path.begin(); it2 != path.end(); ++it2)
        {
            rpc::v1::PathElement* proto_element = proto_path->add_elements();
            STPathElement const & elt = *it2;
        
            if(elt.isOffer())
            {
                if(elt.hasCurrency())
                {
                   Currency const & currency = elt.getCurrency(); 
                   proto_element->set_currency(to_string(currency));
                }
                if(elt.hasIssuer())
                {
                    AccountID const & issuer = elt.getIssuerID();
                    proto_element->set_issuer(toBase58(issuer));
                }
            }
            AccountID const & path_account = elt.getAccountID();
            proto_element->set_account(toBase58(path_account));
        }
    }
}

//helper
std::string ledgerEntryTypeString(std::uint16_t type)
{
    return LedgerFormats::getInstance().findByType(
            safe_cast<LedgerEntryType>(type))->getName();
}

//helper
std::string txnTypeString(TxType type)
{
    return TxFormats::getInstance().findByType(type)->getName();
}

template <class T>
void populateFields(T& proto,STObject const& obj, std::uint16_t type)
{
    //TODO are these all of the ledger entry types that Payment could create?
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
    else
    {
        //Ledger object not supported
    }
}

void populateMeta(rpc::v1::Meta& proto, std::shared_ptr<TxMeta> txMeta)
{
    proto.set_transaction_index(txMeta->getIndex());
    proto.set_transaction_result(
            transToken(txMeta->getResultTER()));

    if(txMeta->hasDeliveredAmount())
    {
        populateAmount(*proto.mutable_delivered_amount(),
                txMeta->getDeliveredAmount());
    }

    STArray& nodes = txMeta->getNodes();
    for(auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        STObject & obj = *it;
        rpc::v1::AffectedNode* node =
            proto.add_affected_node();

        //ledger index
        uint256 ledger_index = obj.getFieldH256(sfLedgerIndex);
        node->set_ledger_index(toBytes(ledger_index));

        //ledger entry type
        std::uint16_t type = obj.getFieldU16(sfLedgerEntryType);
        std::string type_str = ledgerEntryTypeString(type);
        node->set_ledger_entry_type(type_str);

        //modified node
        if(obj.getFName() == sfModifiedNode)
        {
            //final fields
            if(obj.isFieldPresent(sfFinalFields))
            {
                STObject& final_fields =
                    obj.getField(sfFinalFields).downcast<STObject>();

                rpc::v1::LedgerObject* final_fields_proto =
                    node->mutable_modified_node()->mutable_final_fields();

                populateFields(*final_fields_proto,final_fields,type);
            }
            //previous fields
            if(obj.isFieldPresent(sfPreviousFields))
            {
                STObject& prev_fields =
                    obj.getField(sfPreviousFields).downcast<STObject>();

                rpc::v1::LedgerObject* prev_fields_proto =
                    node->mutable_modified_node()->mutable_previous_fields();

                populateFields(*prev_fields_proto,prev_fields,type);
            }

            //prev txn id and prev txn ledger seq
            uint256 prev_txn_id = obj.getFieldH256(sfPreviousTxnID);
            node->mutable_modified_node()->
                set_previous_txn_id(toBytes(prev_txn_id));

            node->mutable_modified_node()->
                set_previous_txn_ledger_sequence(
                        obj.getFieldU32(sfPreviousTxnLgrSeq));

        }
        //created node
        else if(obj.getFName() == sfCreatedNode)
        {
            //new fields
            if(obj.isFieldPresent(sfNewFields))
            {
                STObject& new_fields =
                    obj.getField(sfNewFields).downcast<STObject>();

                rpc::v1::LedgerObject* new_fields_proto =
                    node->mutable_created_node()->mutable_new_fields();

                populateFields(*new_fields_proto,new_fields,type);
            }
        }
        //deleted node
        else if(obj.getFName() == sfDeletedNode)
        {
            //final fields
            if(obj.isFieldPresent(sfFinalFields))
            {
                STObject& final_fields =
                    obj.getField(sfNewFields).downcast<STObject>();

                rpc::v1::LedgerObject* final_fields_proto =
                    node->mutable_deleted_node()->mutable_final_fields();

                populateFields(*final_fields_proto,final_fields,type);
            }
        }
    }
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
        populateTransaction(*result.mutable_tx(),st_txn);
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

                populateMeta(*result.mutable_meta(), txMeta);
            }
        }
    }
    return {result, status};
}

} // ripple
