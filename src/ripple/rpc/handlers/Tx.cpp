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
std::string toBytes(T const & data)
{
    const char* bytes = reinterpret_cast<const char*>(data.data());
    return {bytes,data.size()};
}

template <class T>
void populateAmount(T& proto,STAmount const& amount)
{
    if(amount.native())
    {
        proto.mutable_xrp_amount()->
            set_drops(amount.xrp().drops());
    }
    else
    {
        io::xpring::FiatAmount* fiat =
            proto.mutable_fiat_amount();
        Issue const & issue = amount.issue();
        Currency currency = issue.currency;
        fiat->mutable_currency()->set_name(to_string(issue.currency));
        fiat->mutable_currency()->set_code(toBytes(currency));
        fiat->set_value(to_string(amount.iou()));
        fiat->set_issuer(toBase58(issue.account));
    }
}

void populateTransaction(
        io::xpring::Transaction* txn_proto,
        std::shared_ptr<STTx const> txn_st)
{
    STAmount fee = txn_st->getFieldAmount(sfFee);
    txn_proto->mutable_fee()->set_drops(fee.xrp().drops());


    STAmount amount = txn_st->getFieldAmount(sfAmount);
    populateAmount(*txn_proto->mutable_payment()->mutable_amount(),amount);
    AccountID account_dest = txn_st->getAccountID(sfDestination);
    txn_proto->mutable_payment()->set_destination(toBase58(account_dest));
//    if(amount.native())
//    {
//    
//        txn_proto->mutable_payment()->mutable_xrp_amount()->set_drops(std::to_string(amount.xrp().drops()));
//    }
//    else
//    {
//        txn_proto->mutable_payment()->mutable_fiat_amount()->set_value(to_string(amount.iou()));
//        txn_proto->mutable_payment()->mutable_fiat_amount()->set_issuer(toBase58(amount.issue().account));
//        io::xpring::Currency* currency_proto = txn_proto->mutable_payment()->mutable_fiat_amount()->mutable_currency();
//        Currency currency = amount.issue().currency;
//        currency_proto->set_name(to_string(currency));
//
//        const char* currency_bytes = reinterpret_cast<const char*>(currency.data());
//        std::string currency_str{currency_bytes,currency.size()};
//
//        currency_proto->set_code(currency_str);
//
//    }


    //TODO return account as bytes or base58 string?
    AccountID account = txn_st->getAccountID(sfAccount);
    txn_proto->set_account(toBase58(account));
//    const char* account_data = reinterpret_cast<const char*>(account.data());
//    std::string account_str{account_data,account.size()};

    //std::cout << "account is " << toBase58(account) << std::endl;



//    const char* account_dest_data = reinterpret_cast<const char*>(account_dest.data());
//    std::string account_dest_str{account_dest_data,account_dest.size()};


    txn_proto->set_sequence(txn_st->getFieldU32(sfSequence));

    Blob signingPubKey = txn_st->getFieldVL(sfSigningPubKey);
//    std::string pubKeyStr{reinterpret_cast<const char*>(signingPubKey.data()),signingPubKey.size()};
    txn_proto->set_signing_public_key_hex(toBytes(signingPubKey));

    txn_proto->set_flags(txn_st->getFieldU32(sfFlags));

    txn_proto->set_last_ledger_sequence(txn_st->getFieldU32(sfLastLedgerSequence));

    STPathSet const & pathset = txn_st->getFieldPathSet(sfPaths);

    for(auto it = pathset.begin(); it < pathset.end(); ++it)
    {
        STPath const & path = *it;

        io::xpring::Path* proto_path = txn_proto->add_paths();

        for(auto it2 = path.begin(); it2 != path.end(); ++it2)
        {
            io::xpring::PathElement* proto_element = proto_path->add_elements();

            STPathElement const & elt = *it2;
        
            //TODO is this correct? 
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

    if(txn_st->isFieldPresent(sfSendMax))
    {

        STAmount const & send_max = txn_st->getFieldAmount(sfSendMax);

        populateAmount(*txn_proto->mutable_send_max(),send_max);

    }

//   if(send_max.native())
//   {
//       //TODO: shouldnt happen
//       
//       std::cout << "send max is native" << std::endl;
//
//        
//
//       io::xpring::XRPAmount* native_send_max = txn_proto->mutable_send_max_native();
//       native_send_max->set_drops(std::to_string(send_max.xrp().drops()));
//    
//   }
//   else
//   {
//    Issue const & issue = send_max.issue();
//
//    io::xpring::FiatAmount* fiat_send_max = txn_proto->mutable_send_max_fiat();
//    fiat_send_max->set_issuer(toBase58(issue.account));
//    io::xpring::Currency* currency_proto = fiat_send_max->mutable_currency();
//    Currency currency = issue.currency;
//    currency_proto->set_name(to_string(currency));
//
//    const char* currency_bytes = reinterpret_cast<const char*>(currency.data());
//    std::string currency_str{currency_bytes,currency.size()};
//
//    currency_proto->set_code(currency_str);
//
//    fiat_send_max->set_value(to_string(send_max.iou()));
//   }

   Blob blob = txn_st->getFieldVL(sfTxnSignature);


   txn_proto->set_signature(toBytes(blob));
}



void populateFields(io::xpring::LedgerFields& fields_proto, STObject& fields_st)
{

    //AccountRoot fields
    if(fields_st.isFieldPresent(sfAccount))
    {
        AccountID account = fields_st.getAccountID(sfAccount);
        fields_proto.set_account(toBase58(account));
    }
    if(fields_st.isFieldPresent(sfBalance))
    {
        STAmount amount = fields_st.getFieldAmount(sfBalance);
        populateAmount(*fields_proto.mutable_balance(),amount);
    }
    if(fields_st.isFieldPresent(sfSequence))
    {
        fields_proto.set_sequence(fields_st.getFieldU32(sfSequence));
    }
    if(fields_st.isFieldPresent(sfFlags))
    {
        fields_proto.set_flags(fields_st.getFieldU32(sfFlags));
    }
    if(fields_st.isFieldPresent(sfOwnerCount))
    {
        fields_proto.set_owner_count(fields_st.getFieldU32(sfOwnerCount)); 
    }
    if(fields_st.isFieldPresent(sfPreviousTxnID))
    {
        fields_proto.set_previous_txn_id(
                toBytes(fields_st.getFieldH256(sfPreviousTxnID)));
    }
    if(fields_st.isFieldPresent(sfPreviousTxnLgrSeq))
    {
        fields_proto.set_previous_txn_ledger_sequence(
                fields_st.getFieldU32(sfPreviousTxnLgrSeq));
    }
    if(fields_st.isFieldPresent(sfAccountTxnID))
    {
        fields_proto.set_account_txn_id(
                toBytes(fields_st.getFieldH256(sfAccountTxnID)));
    }
    if(fields_st.isFieldPresent(sfDomain))
    {
        fields_proto.set_domain(toBytes(fields_st.getFieldVL(sfDomain))); 
    }
    if(fields_st.isFieldPresent(sfEmailHash))
    {
        fields_proto.set_email_hash(toBytes(fields_st.getFieldH128(sfEmailHash)));
    }
    if(fields_st.isFieldPresent(sfMessageKey))
    {
        fields_proto.set_message_key(toBytes(fields_st.getFieldVL(sfMessageKey)));
    }
    if(fields_st.isFieldPresent(sfRegularKey))
    {
        fields_proto.set_regular_key(toBytes(fields_st.getFieldVL(sfRegularKey)));
    }
    if(fields_st.isFieldPresent(sfTickSize))
    {
        fields_proto.set_tick_size(fields_st.getFieldU8(sfTickSize));
    }
    if(fields_st.isFieldPresent(sfTransferRate))
    {
        fields_proto.set_transfer_rate(fields_st.getFieldU32(sfTransferRate));
    }
    //Offer fields

    if(fields_st.isFieldPresent(sfTakerPays))
    {
        STAmount amount = fields_st.getFieldAmount(sfTakerPays);
        populateAmount(*fields_proto.mutable_taker_pays(),amount);
    }
    if(fields_st.isFieldPresent(sfTakerGets))
    {
        STAmount amount = fields_st.getFieldAmount(sfTakerGets);
        populateAmount(*fields_proto.mutable_taker_gets(),amount);
    }
    //TODO: do we need to handle the below fields? What is the difference 
    //between the below fields and the above two fields?
    //sfTakerPaysCurrency, sfTakerPaysIssuer,
    //sfTakerGetsCurrency, sfTakerGetsIssuer


    if(fields_st.isFieldPresent(sfBookDirectory))
    {
        fields_proto.set_book_directory(
                toBytes(fields_st.getFieldVL(sfBookDirectory)));
    }
    if(fields_st.isFieldPresent(sfBookNode))
    {
        fields_proto.set_book_node(fields_st.getFieldU64(sfBookNode));
    }
    if(fields_st.isFieldPresent(sfExpiration))
    {
        fields_proto.set_expiration(fields_st.getFieldU32(sfExpiration));
    }

    //RippleState fields
    if(fields_st.isFieldPresent(sfLowLimit))
    {
        STAmount amount = fields_st.getFieldAmount(sfLowLimit);
        populateAmount(*fields_proto.mutable_low_limit(),amount);
    }
    if(fields_st.isFieldPresent(sfHighLimit))
    {
        STAmount amount = fields_st.getFieldAmount(sfHighLimit);
        populateAmount(*fields_proto.mutable_high_limit(),amount);
    }
    if(fields_st.isFieldPresent(sfLowNode))
    {
        fields_proto.set_low_node(fields_st.getFieldU64(sfLowNode));
    }
    if(fields_st.isFieldPresent(sfHighNode))
    {
        fields_proto.set_high_node(fields_st.getFieldU64(sfHighNode));
    }
    if(fields_st.isFieldPresent(sfLowQualityIn))
    {
        fields_proto.set_low_quality_in(fields_st.getFieldU32(sfLowQualityIn));
    }
    if(fields_st.isFieldPresent(sfLowQualityOut))
    {
        fields_proto.set_low_quality_out(fields_st.getFieldU32(sfLowQualityOut));
    }
    if(fields_st.isFieldPresent(sfHighQualityIn))
    {
        fields_proto.set_high_quality_in(fields_st.getFieldU32(sfHighQualityIn));
    }
    if(fields_st.isFieldPresent(sfHighQualityOut))
    {
        fields_proto.set_high_quality_out(fields_st.getFieldU32(sfHighQualityOut));
    }
}

std::string ledgerEntryTypeString(std::uint16_t type)
{
    if(type == LedgerEntryType::ltACCOUNT_ROOT)
    {
        return "AccountRoot";

    } else if(type == LedgerEntryType::ltRIPPLE_STATE)
    {
        return "RippleState";
    }
    else if(type == LedgerEntryType::ltOFFER)
    {
        return "Offer";
    }
    else
    {
        //TODO are there other ledger entry types that Payment
        //transactions can affect?
        std::cout << "not handled ledger entry type: "
            << std::to_string(type) << std::endl;

        return ""; //Not supported yet
    }
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
            if(txMeta->hasDeliveredAmount())
            {
                populateAmount(*result.mutable_meta()->mutable_delivered_amount(),
                        txMeta->getDeliveredAmount());
            }

            STArray& nodes = txMeta->getNodes();

            for(auto it = nodes.begin(); it != nodes.end(); ++it)
            {
                STObject & obj = *it;
                std::cout << "processing node" << std::endl;

                for(size_t i = 0; i < obj.getCount(); ++i)
                {
                    SField const& field = obj.getFieldSType(i);
                    std::cout << "field name is " << field.getName() << std::endl;

                    std::cout << obj.peekAtField(field).getFullText() << std::endl;
                }

                std::cout << "fname is " << obj.getFName().getName() << std::endl;

                io::xpring::AffectedNode* node = result.mutable_meta()->add_affected_node();

                //ledger index
                uint256 ledger_index = obj.getFieldH256(sfLedgerIndex);
                node->set_ledger_index(toBytes(ledger_index));

                //ledger entry type
                std::uint16_t type = obj.getFieldU16(sfLedgerEntryType);
                std::string type_str = ledgerEntryTypeString(type);
                if(type_str == "")
                {
                    continue;
                }
                else
                {
                    node->set_ledger_entry_type(type_str);
                }

                //modified node
                if(obj.getFName() == sfModifiedNode)
                {
                    std::cout << "modified is present" << std::endl;

                    //final fields
                    if(obj.isFieldPresent(sfFinalFields))
                    {
                        std::cout << "final fields is present" << std::endl;

                        STObject& final_fields =
                            obj.getField(sfFinalFields).downcast<STObject>();

                        io::xpring::LedgerFields* final_fields_proto =
                            node->mutable_modified_node()->mutable_final_fields();

                        populateFields(*final_fields_proto,final_fields);

                    }
                    //previous fields
                    if(obj.isFieldPresent(sfPreviousFields))
                    {
                        std::cout << "previous fields is present" << std::endl;
                        STObject& prev_fields = obj.getField(sfPreviousFields).downcast<STObject>();
                        io::xpring::LedgerFields* prev_fields_proto =
                            node->mutable_modified_node()->mutable_previous_fields();

                        populateFields(*prev_fields_proto,prev_fields);

                    }
                    std::cout << "setting prev txn id and sequence" << std::endl;

                    //prev txn id and prev txn ledger seq
                    uint256 prev_txn_id = obj.getFieldH256(sfPreviousTxnID);
                    node->mutable_modified_node()->
                        set_previous_txn_id(toBytes(prev_txn_id));

                    node->mutable_modified_node()->
                        set_previous_txn_ledger_sequence(
                                obj.getFieldU32(sfPreviousTxnLgrSeq));

                    std::cout << "done" << std::endl;
                }
                //created node
                else if(obj.getFName() == sfCreatedNode)
                {
                    //new fields
                    if(obj.isFieldPresent(sfNewFields))
                    {
                        STObject& new_fields =
                            obj.getField(sfNewFields).downcast<STObject>();

                        io::xpring::LedgerFields* new_fields_proto =
                            node->mutable_created_node()->mutable_new_fields();

                        populateFields(*new_fields_proto,new_fields);
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

                        io::xpring::LedgerFields* final_fields_proto =
                            node->mutable_deleted_node()->mutable_final_fields();

                        populateFields(*final_fields_proto,final_fields);

                    }
                }



            }

        }
    }

    return {result, status};

}

} // ripple
