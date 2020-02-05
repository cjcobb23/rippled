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

#include <ripple/rpc/impl/GRPCHelpers.h>
namespace ripple {
namespace RPC {

// In the below populateProto* functions, getProto is a function that returns
// a reference to the mutable protobuf message to be populated. The reason this
// is a function, as opposed to just a reference to the object, is that getting
// a reference to the proto object (via something like
// proto.mutable_clear_flag()), default initializes the proto object. However,
// if the corresponding field is not present in the STObject, we don't want to
// initialize the proto object, To get around this, getProto is a function that
// is called only if the field is present in the STObject
template <class T>
void
populateProtoU8(STObject const& obj, SF_U8 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldU8(field));
    }
}

template <class T>
void
populateProtoU16(STObject const& obj, SF_U16 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldU16(field));
    }
}

template <class T>
void
populateProtoU32(STObject const& obj, SF_U32 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldU32(field));
    }
}

template <class T>
void
populateProtoU64(STObject const& obj, SF_U64 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldU64(field));
    }
}

template <class T>
void
populateProtoVL(STObject const& obj, SF_Blob const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        auto data = obj.getFieldVL(field);
        getProto()->set_value(data.data(), data.size());
    }
}

template <class T>
void
populateProtoVLasString(
    STObject const& obj,
    SF_Blob const& field,
    T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        auto data = obj.getFieldVL(field);
        getProto()->set_value(
            reinterpret_cast<const char*>(data.data()), data.size());
    }
}

template <class T>
void
populateProtoH128(STObject const& obj, SF_U128 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldH128(field).data(), uint128::size());
    }
}

template <class T>
void
populateProtoH160(STObject const& obj, SF_U160 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldH160(field).data(), uint160::size());
    }
}

template <class T>
void
populateProtoH256(STObject const& obj, SF_U256 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldH256(field).data(), uint256::size());
    }
}

template <class T>
void
populateProtoVec256(
    STObject const& obj,
    SF_Vec256 const& field,
    T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        const STVector256& vec = obj.getFieldV256(field);
        for (size_t i = 0; i < vec.size(); ++i)
        {
            uint256 const& elt = vec[i];
            getProto()->set_value(elt.data(), elt.size());
        }
    }
}

template <class T>
void
populateProtoAccount(
    STObject const& obj,
    SF_Account const& field,
    T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->mutable_value()->set_address(
            toBase58(obj.getAccountID(field)));
    }
}

template <class T>
void
populateProtoAmount(
    STObject const& obj,
    SF_Amount const& field,
    T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        auto amount = obj.getFieldAmount(field);
        populateProtoAmount(amount, *getProto());
    }
}

template <class T>
void
populateClearFlag(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfClearFlag, [&proto]() { return proto.mutable_clear_flag(); });
}

template <class T>
void
populateDomain(STObject const& obj, T& proto)
{
    populateProtoVLasString(
        obj, sfDomain, [&proto]() { return proto.mutable_domain(); });
}

template <class T>
void
populateEmailHash(STObject const& obj, T& proto)
{
    populateProtoH128(
        obj, sfEmailHash, [&proto]() { return proto.mutable_email_hash(); });
}

template <class T>
void
populateMessageKey(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfMessageKey, [&proto]() { return proto.mutable_message_key(); });
}

template <class T>
void
populateSetFlag(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfSetFlag, [&proto]() { return proto.mutable_set_flag(); });
}

template <class T>
void
populateTransferRate(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfTransferRate, [&proto]() {
        return proto.mutable_transfer_rate();
    });
}

template <class T>
void
populateTickSize(STObject const& obj, T& proto)
{
    populateProtoU8(
        obj, sfTickSize, [&proto]() { return proto.mutable_tick_size(); });
}

template <class T>
void
populateExpiration(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfExpiration, [&proto]() { return proto.mutable_expiration(); });
}

template <class T>
void
populateOfferSequence(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfOfferSequence, [&proto]() {
        return proto.mutable_offer_sequence();
    });
}

template <class T>
void
populateTakerGets(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfTakerGets, [&proto]() { return proto.mutable_taker_gets(); });
}

template <class T>
void
populateTakerPays(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfTakerPays, [&proto]() { return proto.mutable_taker_pays(); });
}

template <class T>
void
populateDestination(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfDestination, [&proto]() { return proto.mutable_destination(); });
}

template <class T>
void
populateCheckID(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfCheckID, [&proto]() { return proto.mutable_check_id(); });
}

template <class T>
void
populateAmount(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfAmount, [&proto]() { return proto.mutable_amount(); });
}

template <class T>
void
populateDeliverMin(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfDeliverMin, [&proto]() { return proto.mutable_deliver_min(); });
}

template <class T>
void
populateSendMax(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfSendMax, [&proto]() { return proto.mutable_send_max(); });
}

template <class T>
void
populateDeliveredAmount(STObject const& obj, T& proto)
{
    populateProtoAmount(obj, sfDeliveredAmount, [&proto]() {
        return proto.mutable_delivered_amount();
    });
}

template <class T>
void
populateDestinationTag(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfDestinationTag, [&proto]() {
        return proto.mutable_destination_tag();
    });
}

template <class T>
void
populateInvoiceID(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfInvoiceID, [&proto]() { return proto.mutable_invoice_id(); });
}

template <class T>
void
populateAuthorize(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfAuthorize, [&proto]() { return proto.mutable_authorize(); });
}

template <class T>
void
populateUnauthorize(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfUnauthorize, [&proto]() { return proto.mutable_unauthorize(); });
}

template <class T>
void
populateOwner(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfOwner, [&proto]() { return proto.mutable_owner(); });
}

template <class T>
void
populateCancelAfter(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfCancelAfter, [&proto]() {
        return proto.mutable_cancel_after();
    });
}

template <class T>
void
populateFinishAfter(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfFinishAfter, [&proto]() {
        return proto.mutable_finish_after();
    });
}

template <class T>
void
populateCondition(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfCondition, [&proto]() { return proto.mutable_condition(); });
}

template <class T>
void
populateFulfillment(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfFulfillment, [&proto]() { return proto.mutable_fulfillment(); });
}

template <class T>
void
populateChannel(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfPayChannel, [&proto]() { return proto.mutable_channel(); });
}

template <class T>
void
populateBalance(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfBalance, [&proto]() { return proto.mutable_balance(); });
}

template <class T>
void
populateSignature(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfSignature, [&proto]() { return proto.mutable_signature(); });
}

template <class T>
void
populatePublicKey(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfPublicKey, [&proto]() { return proto.mutable_public_key(); });
}

template <class T>
void
populateSettleDelay(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfSettleDelay, [&proto]() {
        return proto.mutable_settle_delay();
    });
}

template <class T>
void
populateRegularKey(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfRegularKey, [&proto]() { return proto.mutable_regular_key(); });
}

template <class T>
void
populateSignerQuorum(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfSignerQuorum, [&proto]() {
        return proto.mutable_signer_quorum();
    });
}
template <class T>
void
populateLimitAmount(STObject const& obj, T& proto)
{
    populateProtoAmount(obj, sfLimitAmount, [&proto]() {
        return proto.mutable_limit_amount();
    });
}
template <class T>
void
populateQualityIn(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfQualityIn, [&proto]() { return proto.mutable_quality_in(); });
}

template <class T>
void
populateQualityOut(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfQualityOut, [&proto]() { return proto.mutable_quality_out(); });
}

template <class T>
void
populateAccount(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfAccount, [&proto]() { return proto.mutable_account(); });
}

template <class T>
void
populateFee(STObject const& obj, T& proto)
{
    if (obj.isFieldPresent(sfFee))
    {
        proto.mutable_fee()->set_drops(obj.getFieldAmount(sfFee).xrp().drops());
    }
}

template <class T>
void
populateSigningPublicKey(STObject const& obj, T& proto)
{
    populateProtoVL(obj, sfSigningPubKey, [&proto]() {
        return proto.mutable_signing_public_key();
    });
}

template <class T>
void
populateTransactionSignature(STObject const& obj, T& proto)
{
    populateProtoVL(obj, sfTxnSignature, [&proto]() {
        return proto.mutable_transaction_signature();
    });
}

template <class T>
void
populateFlags(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfFlags, [&proto]() { return proto.mutable_flags(); });
}

template <class T>
void
populateLastLedgerSequence(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfLastLedgerSequence, [&proto]() {
        return proto.mutable_last_ledger_sequence();
    });
}

template <class T>
void
populateSourceTag(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfSourceTag, [&proto]() { return proto.mutable_source_tag(); });
}

template <class T>
void
populateAccountTransactionID(STObject const& obj, T& proto)
{
    populateProtoH256(obj, sfAccountTxnID, [&proto]() {
        return proto.mutable_account_transaction_id();
    });
}

template <class T>
void
populateMemoData(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfMemoData, [&proto]() { return proto.mutable_memo_data(); });
}

template <class T>
void
populateMemoFormat(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfMemoFormat, [&proto]() { return proto.mutable_memo_format(); });
}

template <class T>
void
populateMemoType(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfMemoType, [&proto]() { return proto.mutable_memo_type(); });
}

template <class T>
void
populateSequence(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfSequence, [&proto]() { return proto.mutable_sequence(); });
}

template <class T, class R>
void
populateProtoArray(
    STObject const& obj,
    SField const& outerField,
    SField const& innerField,
    T const& getProto,
    R const& populateProto)
{
    if (obj.isFieldPresent(outerField) &&
        obj.peekAtField(outerField).getSType() == SerializedTypeID::STI_ARRAY)
    {
        auto arr = obj.getFieldArray(outerField);
        for (auto it = arr.begin(); it != arr.end(); ++it)
        {
            populateProto(*it, *getProto());
        }
    }
}

template <class T>
void
populateSignerWeight(STObject const& obj, T& proto)
{
    populateProtoU16(obj, sfSignerWeight, [&proto]() {
        return proto.mutable_signer_weight();
    });
}

template <class T>
void
populateSignerEntries(STObject const& obj, T& proto)
{
    populateProtoArray(
        obj,
        sfSignerEntries,
        sfSignerEntry,
        [&proto]() { return proto.add_signer_entries(); },
        [](auto& innerObj, auto& innerProto) {
            populateAccount(innerObj, innerProto);
            populateSignerWeight(innerObj, innerProto);
        });
}

template <class T>
void
populateMemos(STObject const& obj, T& proto)
{
    populateProtoArray(
        obj,
        sfMemos,
        sfMemo,
        [&proto]() { return proto.add_memos(); },
        [](auto& innerObj, auto& innerProto) {
            populateMemoData(innerObj, innerProto);
            populateMemoType(innerObj, innerProto);
            populateMemoFormat(innerObj, innerProto);
        });
}

template <class T>
void
populateSigners(STObject const& obj, T& proto)
{
    populateProtoArray(
        obj,
        sfSigners,
        sfSigner,
        [&proto]() { return proto.add_signers(); },
        [](auto& innerObj, auto& innerProto) {
            populateAccount(innerObj, innerProto);
            populateTransactionSignature(innerObj, innerProto);
            populateSigningPublicKey(innerObj, innerProto);
        });
}


template <class T>
void
populateOwnerCount(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfOwnerCount, [&proto]() { return proto.mutable_owner_count(); });
}

template <class T>
void
populatePreviousTransactionID(STObject const& obj, T& proto)
{
    populateProtoH256(obj, sfPreviousTxnID, [&proto]() {
        return proto.mutable_previous_transaction_id();
    });
}

template <class T>
void
populatePreviousTransactionLedgerSequence(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfPreviousTxnLgrSeq, [&proto]() {
        return proto.mutable_previous_transaction_ledger_sequence();
    });
}

template <class T>
void
populateLowLimit(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfLowLimit, [&proto]() { return proto.mutable_low_limit(); });
}

template <class T>
void
populateHighLimit(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfHighLimit, [&proto]() { return proto.mutable_high_limit(); });
}

template <class T>
void
populateLowNode(STObject const& obj, T& proto)
{
    populateProtoU64(
        obj, sfLowNode, [&proto]() { return proto.mutable_low_node(); });
}

template <class T>
void
populateHighNode(STObject const& obj, T& proto)
{
    populateProtoU64(
        obj, sfHighNode, [&proto]() { return proto.mutable_high_node(); });
}

template <class T>
void
populateLowQualityIn(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfLowQualityIn, [&proto]() {
        return proto.mutable_low_quality_in();
    });
}

template <class T>
void
populateLowQualityOut(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfLowQualityOut, [&proto]() {
        return proto.mutable_low_quality_out();
    });
}

template <class T>
void
populateHighQualityIn(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfHighQualityIn, [&proto]() {
        return proto.mutable_high_quality_in();
    });
}

template <class T>
void
populateHighQualityOut(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfHighQualityOut, [&proto]() {
        return proto.mutable_high_quality_out();
    });
}

template <class T>
void
populateBookDirectory(STObject const& obj, T& proto)
{
    populateProtoH256(obj, sfBookDirectory, [&proto]() {
        return proto.mutable_book_directory();
    });
}

template <class T>
void
populateBookNode(STObject const& obj, T& proto)
{
    populateProtoU64(
        obj, sfBookNode, [&proto]() { return proto.mutable_book_node(); });
}

template <class T>
void
populateOwnerNode(STObject const& obj, T& proto)
{
    populateProtoU64(
        obj, sfOwnerNode, [&proto]() { return proto.mutable_owner_node(); });
}

template <class T>
void
populateSignerListID(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfSignerListID, [&proto]() {
        return proto.mutable_signer_list_id();
    });
}

template <class T>
void
populateAmendment(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfAmendment, [&proto]() { return proto.mutable_amendment(); });
}

template <class T>
void
populateCloseTime(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfCloseTime, [&proto]() { return proto.mutable_close_time(); });
}

template <class T>
void
populateMajorities(STObject const& obj, T& proto)
{
    populateProtoArray(
        obj,
        sfMajorities,
        sfMajority,
        [&proto]() { return proto.add_majorities(); },
        [](auto innerObj, auto innerProto) {
            populateAmendment(innerObj, innerProto);
            populateCloseTime(innerObj, innerProto);
        });
}

template <class T>
void
populateAmendments(STObject const& obj, T& proto)
{
    populateProtoVec256(
        obj, sfAmendments, [&proto]() { return proto.add_amendments(); });
}

template <class T>
void
populateHashes(STObject const& obj, T& proto)
{
    populateProtoVec256(
        obj, sfHashes, [&proto]() { return proto.add_hashes(); });
}

template <class T>
void
populateIndexes(STObject const& obj, T& proto)
{
    populateProtoVec256(
        obj, sfIndexes, [&proto]() { return proto.add_indexes(); });
}

template <class T>
void
populateRootIndex(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfRootIndex, [&proto]() { return proto.mutable_root_index(); });
}

template <class T>
void
populateIndexNext(STObject const& obj, T& proto)
{
    populateProtoU64(
        obj, sfIndexNext, [&proto]() { return proto.mutable_index_next(); });
}

template <class T>
void
populateIndexPrevious(STObject const& obj, T& proto)
{
    populateProtoU64(obj, sfIndexPrevious, [&proto]() {
        return proto.mutable_index_previous();
    });
}

template <class T>
void
populateProtoCurrency(
    STObject const& obj,
    SF_U160 const& field,
    T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        auto cur = obj.getFieldH160(field);
        auto proto = getProto()->mutable_value();
        proto->set_code(cur.data(), cur.size());
        proto->set_name(to_string(cur));
    }
}

template <class T>
void
populateTakerPaysCurrency(STObject const& obj, T& proto)
{
    populateProtoCurrency(obj, sfTakerPaysCurrency, [&proto]() {
        return proto.mutable_taker_pays_currency();
    });
}

template <class T>
void
populateTakerPaysIssuer(STObject const& obj, T& proto)
{
    populateProtoH160(obj, sfTakerPaysIssuer, [&proto]() {
        return proto.mutable_taker_pays_issuer();
    });
}

template <class T>
void
populateTakerGetsCurrency(STObject const& obj, T& proto)
{
    populateProtoCurrency(obj, sfTakerGetsCurrency, [&proto]() {
        return proto.mutable_taker_gets_currency();
    });
}

template <class T>
void
populateTakerGetsIssuer(STObject const& obj, T& proto)
{
    populateProtoH160(obj, sfTakerGetsIssuer, [&proto]() {
        return proto.mutable_taker_gets_issuer();
    });
}

template <class T>
void
populateDestinationNode(STObject const& obj, T& proto)
{
    populateProtoU64(obj, sfDestinationNode, [&proto]() {
        return proto.mutable_destination_node();
    });
}

template <class T>
void
populateBaseFee(STObject const& obj, T& proto)
{
    populateProtoU64(
        obj, sfBaseFee, [&proto]() { return proto.mutable_base_fee(); });
}

template <class T>
void
populateReferenceFeeUnits(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfReferenceFeeUnits, [&proto]() {
        return proto.mutable_reference_fee_units();
    });
}

template <class T>
void
populateReserveBase(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfReserveBase, [&proto]() {
        return proto.mutable_reserve_base();
    });
}

template <class T>
void
populateReserveIncrement(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfReserveIncrement, [&proto]() {
        return proto.mutable_reserve_increment();
    });
}

void
populateTransactionResultType(
    org::xrpl::rpc::v1::TransactionResult& proto,
    TER result)
{
    if (isTecClaim(result))
    {
        proto.set_result_type(
            org::xrpl::rpc::v1::TransactionResult::RESULT_TYPE_TEC);
    }
    if (isTefFailure(result))
    {
        proto.set_result_type(
            org::xrpl::rpc::v1::TransactionResult::RESULT_TYPE_TEF);
    }
    if (isTelLocal(result))
    {
        proto.set_result_type(
            org::xrpl::rpc::v1::TransactionResult::RESULT_TYPE_TEL);
    }
    if (isTemMalformed(result))
    {
        proto.set_result_type(
            org::xrpl::rpc::v1::TransactionResult::RESULT_TYPE_TEM);
    }
    if (isTerRetry(result))
    {
        proto.set_result_type(
            org::xrpl::rpc::v1::TransactionResult::RESULT_TYPE_TER);
    }
    if (isTesSuccess(result))
    {
        proto.set_result_type(
            org::xrpl::rpc::v1::TransactionResult::RESULT_TYPE_TES);
    }
}

void
populateAccountSet(org::xrpl::rpc::v1::AccountSet& proto, STObject const& obj)
{
    populateClearFlag(obj, proto);

    populateDomain(obj, proto);

    populateEmailHash(obj, proto);

    populateMessageKey(obj, proto);

    populateSetFlag(obj, proto);

    populateTransferRate(obj, proto);

    populateTickSize(obj, proto);
}

void
populateOfferCreate(org::xrpl::rpc::v1::OfferCreate& proto, STObject const& obj)
{
    populateExpiration(obj, proto);

    populateOfferSequence(obj, proto);

    populateTakerGets(obj, proto);

    populateTakerPays(obj, proto);
}

void
populateOfferCancel(org::xrpl::rpc::v1::OfferCancel& proto, STObject const& obj)
{
    populateOfferSequence(obj, proto);
}

void
populateAccountDelete(
    org::xrpl::rpc::v1::AccountDelete& proto,
    STObject const& obj)
{
    populateDestination(obj, proto);
}

void
populateCheckCancel(org::xrpl::rpc::v1::CheckCancel& proto, STObject const& obj)
{
    populateCheckID(obj, proto);
}

void
populateCheckCash(org::xrpl::rpc::v1::CheckCash& proto, STObject const& obj)
{
    populateCheckID(obj, proto);

    populateAmount(obj, proto);

    populateDeliverMin(obj, proto);
}

void
populateCheckCreate(org::xrpl::rpc::v1::CheckCreate& proto, STObject const& obj)
{
    populateDestination(obj, proto);

    populateSendMax(obj, proto);

    populateDestinationTag(obj, proto);

    populateExpiration(obj, proto);

    populateInvoiceID(obj, proto);
}

void
populateDepositPreauth(
    org::xrpl::rpc::v1::DepositPreauth& proto,
    STObject const& obj)
{
    populateAuthorize(obj, proto);

    populateUnauthorize(obj, proto);
}

void
populateEscrowCancel(
    org::xrpl::rpc::v1::EscrowCancel& proto,
    STObject const& obj)
{
    populateOwner(obj, proto);

    populateOfferSequence(obj, proto);
}

void
populateEscrowCreate(
    org::xrpl::rpc::v1::EscrowCreate& proto,
    STObject const& obj)
{
    populateAmount(obj, proto);

    populateDestination(obj, proto);

    populateCancelAfter(obj, proto);

    populateFinishAfter(obj, proto);

    populateCondition(obj, proto);

    populateDestinationTag(obj, proto);
}

void
populateEscrowFinish(
    org::xrpl::rpc::v1::EscrowFinish& proto,
    STObject const& obj)
{
    populateOwner(obj, proto);

    populateOfferSequence(obj, proto);

    populateCondition(obj, proto);

    populateFulfillment(obj, proto);
}

void
populatePaymentChannelClaim(
    org::xrpl::rpc::v1::PaymentChannelClaim& proto,
    STObject const& obj)
{
    populateChannel(obj, proto);

    populateBalance(obj, proto);

    populateAmount(obj, proto);

    populateSignature(obj, proto);

    populatePublicKey(obj, proto);
}

void
populatePaymentChannelCreate(
    org::xrpl::rpc::v1::PaymentChannelCreate& proto,
    STObject const& obj)
{
    populateAmount(obj, proto);

    populateDestination(obj, proto);

    populateSettleDelay(obj, proto);

    populatePublicKey(obj, proto);

    populateCancelAfter(obj, proto);

    populateDestinationTag(obj, proto);
}

void
populatePaymentChannelFund(
    org::xrpl::rpc::v1::PaymentChannelFund& proto,
    STObject const& obj)
{
    populateChannel(obj, proto);

    populateAmount(obj, proto);

    populateExpiration(obj, proto);
}

void
populateSetRegularKey(
    org::xrpl::rpc::v1::SetRegularKey& proto,
    STObject const& obj)
{
    populateRegularKey(obj, proto);
}

void
populateSignerListSet(
    org::xrpl::rpc::v1::SignerListSet& proto,
    STObject const& obj)
{
    populateSignerQuorum(obj, proto);

    populateSignerEntries(obj, proto);
}

void
populateTrustSet(org::xrpl::rpc::v1::TrustSet& proto, STObject const& obj)
{
    populateLimitAmount(obj, proto);

    populateQualityIn(obj, proto);

    populateQualityOut(obj, proto);
}

void
populatePayment(org::xrpl::rpc::v1::Payment& proto, STObject const& obj)
{
    populateAmount(obj, proto);

    populateDestination(obj, proto);

    populateDestinationTag(obj, proto);

    populateInvoiceID(obj, proto);

    populateSendMax(obj, proto);

    populateDeliverMin(obj, proto);

    if (obj.isFieldPresent(sfPaths))
    {
        // populate path data
        STPathSet const& pathset = obj.getFieldPathSet(sfPaths);
        for (auto it = pathset.begin(); it < pathset.end(); ++it)
        {
            STPath const& path = *it;

            org::xrpl::rpc::v1::Path* protoPath = proto.add_paths();

            for (auto it2 = path.begin(); it2 != path.end(); ++it2)
            {
                org::xrpl::rpc::v1::PathElement* protoElement =
                    protoPath->add_elements();
                STPathElement const& elt = *it2;

                if (elt.isOffer())
                {
                    if (elt.hasCurrency())
                    {
                        Currency const& currency = elt.getCurrency();
                        protoElement->mutable_currency()->set_name(
                            to_string(currency));
                    }
                    if (elt.hasIssuer())
                    {
                        AccountID const& issuer = elt.getIssuerID();
                        protoElement->mutable_issuer()->set_address(
                            toBase58(issuer));
                    }
                }
                else if (elt.isAccount())
                {
                    AccountID const& pathAccount = elt.getAccountID();
                    protoElement->mutable_account()->set_address(
                        toBase58(pathAccount));
                }
            }
        }
    }
}

void
populateAccountRoot(org::xrpl::rpc::v1::AccountRoot& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateBalance(obj, proto);

    populateSequence(obj, proto);

    populateFlags(obj, proto);

    populateOwnerCount(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateAccountTransactionID(obj, proto);

    populateDomain(obj, proto);

    populateEmailHash(obj, proto);

    populateMessageKey(obj, proto);

    populateRegularKey(obj, proto);

    populateTickSize(obj, proto);

    populateTransferRate(obj, proto);
}

void
populateAmendments(org::xrpl::rpc::v1::Amendments& proto, STObject const& obj)
{
    populateAmendments(obj, proto);

    populateMajorities(obj, proto);
}

void
populateCheck(org::xrpl::rpc::v1::Check& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateDestination(obj, proto);

    populateFlags(obj, proto);

    populateOwnerNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateSendMax(obj, proto);

    populateSequence(obj, proto);

    populateDestinationNode(obj, proto);

    populateDestinationTag(obj, proto);

    populateExpiration(obj, proto);

    populateInvoiceID(obj, proto);

    populateSourceTag(obj, proto);
}

void
populateDepositPreauth(
    org::xrpl::rpc::v1::DepositPreauthObject& proto,
    STObject const& obj)
{
    populateAccount(obj, proto);

    populateAuthorize(obj, proto);

    populateFlags(obj, proto);

    populateOwnerNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);
}

void
populateFeeSettings(org::xrpl::rpc::v1::FeeSettings& proto, STObject const& obj)
{
    populateBaseFee(obj, proto);

    populateReferenceFeeUnits(obj, proto);

    populateReserveBase(obj, proto);

    populateReserveIncrement(obj, proto);

    populateFlags(obj, proto);
}

void
populateEscrow(org::xrpl::rpc::v1::Escrow& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateDestination(obj, proto);

    populateAmount(obj, proto);

    populateCondition(obj, proto);

    populateCancelAfter(obj, proto);

    populateFinishAfter(obj, proto);

    populateFlags(obj, proto);

    populateSourceTag(obj, proto);

    populateDestinationTag(obj, proto);

    populateOwnerNode(obj, proto);

    populateDestinationNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);
}

void
populateLedgerHashes(
    org::xrpl::rpc::v1::LedgerHashes& proto,
    STObject const& obj)
{
    populateLastLedgerSequence(obj, proto);

    populateHashes(obj, proto);

    populateFlags(obj, proto);
}

void
populatePayChannel(org::xrpl::rpc::v1::PayChannel& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateAmount(obj, proto);

    populateBalance(obj, proto);

    populatePublicKey(obj, proto);

    populateSettleDelay(obj, proto);

    populateOwnerNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateFlags(obj, proto);

    populateExpiration(obj, proto);

    populateCancelAfter(obj, proto);

    populateSourceTag(obj, proto);

    populateDestinationTag(obj, proto);
}

void
populateDirectoryNode(
    org::xrpl::rpc::v1::DirectoryNode& proto,
    STObject const& obj)
{
    populateFlags(obj, proto);

    populateRootIndex(obj, proto);

    populateIndexes(obj, proto);

    populateIndexNext(obj, proto);

    populateIndexPrevious(obj, proto);

    populateTakerPaysIssuer(obj, proto);

    populateTakerPaysCurrency(obj, proto);

    populateTakerGetsCurrency(obj, proto);

    populateTakerGetsIssuer(obj, proto);
}

void
populateOffer(org::xrpl::rpc::v1::Offer& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateSequence(obj, proto);

    populateFlags(obj, proto);

    populateTakerPays(obj, proto);

    populateTakerGets(obj, proto);

    populateBookDirectory(obj, proto);

    populateBookNode(obj, proto);
}

void
populateRippleState(org::xrpl::rpc::v1::RippleState& proto, STObject const& obj)
{
    populateBalance(obj, proto);

    populateFlags(obj, proto);

    populateLowNode(obj, proto);

    populateHighNode(obj, proto);

    populateLowQualityIn(obj, proto);

    populateLowQualityOut(obj, proto);

    populateHighQualityIn(obj, proto);

    populateHighQualityOut(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);
}

void
populateSignerList(org::xrpl::rpc::v1::SignerList& proto, STObject const& obj)
{
    populateFlags(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateOwnerNode(obj, proto);

    populateSignerEntries(obj, proto);

    populateSignerQuorum(obj, proto);

    populateSignerListID(obj, proto);
}

void
populateLedgerEntryType(
    org::xrpl::rpc::v1::AffectedNode& proto,
    std::uint16_t lgrType)
{
    switch (lgrType)
    {
        case ltACCOUNT_ROOT:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_ACCOUNT_ROOT);
            break;
        case ltDIR_NODE:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_DIRECTORY_NODE);
            break;
        case ltRIPPLE_STATE:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_RIPPLE_STATE);
            break;
        case ltSIGNER_LIST:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_SIGNER_LIST);
            break;
        case ltOFFER:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_OFFER);
            break;
        case ltLEDGER_HASHES:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_LEDGER_HASHES);
            break;
        case ltAMENDMENTS:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_AMENDMENTS);
            break;
        case ltFEE_SETTINGS:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_FEE_SETTINGS);
            break;
        case ltESCROW:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_ESCROW);
            break;
        case ltPAYCHAN:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_PAY_CHANNEL);
            break;
        case ltCHECK:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_CHECK);
            break;
        case ltDEPOSIT_PREAUTH:
            proto.set_ledger_entry_type(
                org::xrpl::rpc::v1::LEDGER_ENTRY_TYPE_DEPOSIT_PREAUTH);
            break;
    }
}

template <class T>
void
populateLedgerObject(T& proto, STObject& obj, std::uint16_t type)
{
    switch (type)
    {
        case ltACCOUNT_ROOT:
            RPC::populateAccountRoot(*proto.mutable_account_root(), obj);
            break;
        case ltAMENDMENTS:
            RPC::populateAmendments(*proto.mutable_amendments(), obj);
            break;
        case ltDIR_NODE:
            RPC::populateDirectoryNode(*proto.mutable_directory_node(), obj);
            break;
        case ltRIPPLE_STATE:
            RPC::populateRippleState(*proto.mutable_ripple_state(), obj);
            break;
        case ltSIGNER_LIST:
            RPC::populateSignerList(*proto.mutable_signer_list(), obj);
            break;
        case ltOFFER:
            RPC::populateOffer(*proto.mutable_offer(), obj);
            break;
        case ltLEDGER_HASHES:
            RPC::populateLedgerHashes(*proto.mutable_ledger_hashes(), obj);
            break;
        case ltFEE_SETTINGS:
            RPC::populateFeeSettings(*proto.mutable_fee_settings(), obj);
            break;
        case ltESCROW:
            RPC::populateEscrow(*proto.mutable_escrow(), obj);
            break;
        case ltPAYCHAN:
            RPC::populatePayChannel(*proto.mutable_pay_channel(), obj);
            break;
        case ltCHECK:
            RPC::populateCheck(*proto.mutable_check(), obj);
            break;
        case ltDEPOSIT_PREAUTH:
            RPC::populateDepositPreauth(*proto.mutable_deposit_preauth(), obj);
            break;
    }
}

template <class T>
void
populateFields(
    STObject& obj,
    SField const& field,
    uint16_t lgrType,
    T const& getProto)
{
    // final fields
    if (obj.isFieldPresent(field))
    {
        STObject& data = obj.getField(field).downcast<STObject>();

        populateLedgerObject(*getProto(), data, lgrType);
    }
}

template <class T>
void
populateFinalFields(STObject& obj, uint16_t lgrType, T const& getProto)
{
    populateFields(obj, sfFinalFields, lgrType, getProto);
}

template <class T>
void
populatePreviousFields(STObject& obj, uint16_t lgrType, T const& getProto)
{
    populateFields(obj, sfPreviousFields, lgrType, getProto);
}

template <class T>
void
populateNewFields(STObject& obj, uint16_t lgrType, T const& getProto)
{
    populateFields(obj, sfNewFields, lgrType, getProto);
}

void
populateMeta(org::xrpl::rpc::v1::Meta& proto, std::shared_ptr<TxMeta> txMeta)
{
    proto.set_transaction_index(txMeta->getIndex());

    populateTransactionResultType(
        *proto.mutable_transaction_result(), txMeta->getResultTER());
    proto.mutable_transaction_result()->set_result(
        transToken(txMeta->getResultTER()));

    STArray& nodes = txMeta->getNodes();
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        STObject& obj = *it;
        org::xrpl::rpc::v1::AffectedNode* node = proto.add_affected_nodes();

        // ledger index
        uint256 ledgerIndex = obj.getFieldH256(sfLedgerIndex);
        node->set_ledger_index(ledgerIndex.data(), ledgerIndex.size());

        // ledger entry type
        std::uint16_t lgrType = obj.getFieldU16(sfLedgerEntryType);
        populateLedgerEntryType(*node, lgrType);

        // modified node
        if (obj.getFName() == sfModifiedNode)
        {
            populateFinalFields(obj, lgrType, [&node]() {
                return node->mutable_modified_node()->mutable_final_fields();
            });

            populatePreviousFields(obj, lgrType, [&node]() {
                return node->mutable_modified_node()->mutable_previous_fields();
            });

            populatePreviousTransactionID(obj, *node->mutable_modified_node());

            populatePreviousTransactionLedgerSequence(
                obj, *node->mutable_modified_node());
        }
        // created node
        else if (obj.getFName() == sfCreatedNode)
        {
            populateNewFields(obj, lgrType, [&node]() {
                return node->mutable_created_node()->mutable_new_fields();
            });
        }
        // deleted node
        else if (obj.getFName() == sfDeletedNode)
        {
            populateFinalFields(obj, lgrType, [&node]() {
                return node->mutable_deleted_node()->mutable_final_fields();
            });
        }
    }
}

void
populateQueueData(
    org::xrpl::rpc::v1::QueueData& proto,
    std::map<TxSeq, TxQ::AccountTxDetails const> const& txs)
{
    if (!txs.empty())
    {
        proto.set_txn_count(txs.size());
        proto.set_lowest_sequence(txs.begin()->first);
        proto.set_highest_sequence(txs.rbegin()->first);

        boost::optional<bool> anyAuthChanged(false);
        boost::optional<XRPAmount> totalSpend(0);

        for (auto const& [txSeq, txDetails] : txs)
        {
            org::xrpl::rpc::v1::QueuedTransaction& qt =
                *proto.add_transactions();

            qt.set_sequence(txSeq);
            qt.set_fee_level(txDetails.feeLevel);
            if (txDetails.lastValid)
                qt.set_last_ledger_sequence(*txDetails.lastValid);

            if (txDetails.consequences)
            {
                qt.mutable_fee()->set_drops(
                    txDetails.consequences->fee.drops());
                auto spend = txDetails.consequences->potentialSpend +
                    txDetails.consequences->fee;
                qt.mutable_max_spend_drops()->set_drops(spend.drops());
                if (totalSpend)
                    *totalSpend += spend;
                auto authChanged =
                    txDetails.consequences->category == TxConsequences::blocker;
                if (authChanged)
                    anyAuthChanged.emplace(authChanged);
                qt.set_auth_change(authChanged);
            }
            else
            {
                if (anyAuthChanged && !*anyAuthChanged)
                    anyAuthChanged.reset();
                totalSpend.reset();
            }
        }

        if (anyAuthChanged)
            proto.set_auth_change_queued(*anyAuthChanged);
        if (totalSpend)
            proto.mutable_max_spend_drops_total()->set_drops(
                (*totalSpend).drops());
    }
}

void
populateTransaction(
    org::xrpl::rpc::v1::Transaction& proto,
    std::shared_ptr<STTx const> txnSt)
{
    STObject const& obj = *txnSt;

    populateAccount(obj, proto);

    populateFee(obj, proto);

    populateSequence(obj, proto);

    populateSigningPublicKey(obj, proto);

    populateTransactionSignature(obj, proto);

    populateFlags(obj, proto);

    populateLastLedgerSequence(obj, proto);

    populateSourceTag(obj, proto);

    populateAccountTransactionID(obj, proto);

    populateMemos(obj, proto);

    populateSigners(obj, proto);

    auto type = safe_cast<TxType>(txnSt->getFieldU16(sfTransactionType));

    switch (type)
    {
        case TxType::ttPAYMENT:
            populatePayment(*proto.mutable_payment(), *txnSt);
            break;
        case TxType::ttESCROW_CREATE:
            populateEscrowCreate(*proto.mutable_escrow_create(), *txnSt);
            break;
        case TxType::ttESCROW_FINISH:
            populateEscrowFinish(*proto.mutable_escrow_finish(), *txnSt);
            break;
        case TxType::ttACCOUNT_SET:
            populateAccountSet(*proto.mutable_account_set(), *txnSt);
            break;
        case TxType::ttESCROW_CANCEL:
            populateEscrowCancel(*proto.mutable_escrow_cancel(), *txnSt);
            break;
        case TxType::ttREGULAR_KEY_SET:
            populateSetRegularKey(*proto.mutable_set_regular_key(), *txnSt);
            break;
        case TxType::ttOFFER_CREATE:
            populateOfferCreate(*proto.mutable_offer_create(), *txnSt);
            break;
        case TxType::ttOFFER_CANCEL:
            populateOfferCancel(*proto.mutable_offer_cancel(), *txnSt);
            break;
        case TxType::ttSIGNER_LIST_SET:
            populateSignerListSet(*proto.mutable_signer_list_set(), *txnSt);
            break;
        case TxType::ttPAYCHAN_CREATE:
            populatePaymentChannelCreate(
                *proto.mutable_payment_channel_create(), *txnSt);
            break;
        case TxType::ttPAYCHAN_FUND:
            populatePaymentChannelFund(
                *proto.mutable_payment_channel_fund(), *txnSt);
            break;
        case TxType::ttPAYCHAN_CLAIM:
            populatePaymentChannelClaim(
                *proto.mutable_payment_channel_claim(), *txnSt);
            break;
        case TxType::ttCHECK_CREATE:
            populateCheckCreate(*proto.mutable_check_create(), *txnSt);
            break;
        case TxType::ttCHECK_CASH:
            populateCheckCash(*proto.mutable_check_cash(), *txnSt);
            break;
        case TxType::ttCHECK_CANCEL:
            populateCheckCancel(*proto.mutable_check_cancel(), *txnSt);
            break;
        case TxType::ttDEPOSIT_PREAUTH:
            populateDepositPreauth(*proto.mutable_deposit_preauth(), *txnSt);
            break;
        case TxType::ttTRUST_SET:
            populateTrustSet(*proto.mutable_trust_set(), *txnSt);
            break;
        case TxType::ttACCOUNT_DELETE:
            populateAccountDelete(*proto.mutable_account_delete(), *txnSt);
            break;
        default:
            break;
    }
}

/*
template <class T>
void populate*(STObject const& obj, T& proto)
{
    populateProto*(obj, *, [&proto]() {
        return proto.*();
    });
}
*/

}  // namespace RPC
}  // namespace ripple
