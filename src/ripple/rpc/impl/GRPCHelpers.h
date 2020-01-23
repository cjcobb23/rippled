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

#ifndef RIPPLE_RPC_GRPCHELPERS_H_INCLUDED
#define RIPPLE_RPC_GRPCHELPERS_H_INCLUDED

namespace ripple {
namespace RPC {


// In the below populateProto* functions, getProto is a function that returns
// a reference to the mutable protobuf message to be populated. The reason this
// is a function, as opposed to just a reference to the object, is that getting
// a reference to the proto object (via something like proto.mutable_clear_flag()),
// default initializes the proto object. However, if the corresponding field
// is not present in the STObject, we don't want to initialize the proto object,
// To get around this, getProto is a function that is called only if the field
// is present in the STObject
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
populateProtoU32(STObject const& obj, SF_U32 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldU32(field));
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
populateProtoH256(STObject const& obj, SF_U256 const& field, T const& getProto)
{
    if (obj.isFieldPresent(field))
    {
        getProto()->set_value(obj.getFieldH256(field).data(), uint256::size());
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

        if (amount.native())
        {
            getProto()->mutable_value()->mutable_xrp_amount()->set_drops(
                amount.xrp().drops());
        }
        else
        {
            Issue const& issue = amount.issue();

            rpc::v1::IssuedCurrencyAmount* issued =
                getProto()->mutable_value()->mutable_issued_currency_amount();

            issued->mutable_currency()->set_name(to_string(issue.currency));
            issued->mutable_currency()->set_code(
                issue.currency.data(), Currency::size());
            issued->mutable_issuer()->set_address(toBase58(issue.account));
            issued->set_value(to_string(amount.iou()));
        }
    }
}

template <class T>
void populateClearFlag(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfClearFlag, [&proto]() { return proto.mutable_clear_flag(); });
}

template <class T>
void populateDomain(STObject const& obj, T& proto)
{
    populateProtoVLasString(
        obj, sfDomain, [&proto]() { return proto.mutable_domain(); });
}

template <class T>
void populateEmailHash(STObject const& obj, T& proto)
{
    populateProtoH128(
        obj, sfEmailHash, [&proto]() { return proto.mutable_email_hash(); });
}

template <class T>
void populateMessageKey(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfMessageKey, [&proto]() { return proto.mutable_message_key(); });
}

template <class T>
void populateSetFlag(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfSetFlag, [&proto]() { return proto.mutable_set_flag(); });
}

template <class T>
void populateTransferRate(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfTransferRate, [&proto]() {
        return proto.mutable_transfer_rate();
    });
}

template <class T>
void populateTickSize(STObject const& obj, T& proto)
{
    populateProtoU8(
        obj, sfTickSize, [&proto]() { return proto.mutable_tick_size(); });
}

template <class T>
void populateExpiration(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfExpiration, [&proto]() { return proto.mutable_expiration(); });
}


template <class T>
void populateOfferSequence(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfOfferSequence, [&proto]() {
        return proto.mutable_offer_sequence();
    });
}


template <class T>
void populateTakerGets(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfTakerGets, [&proto]() { return proto.mutable_taker_gets(); });
}


template <class T>
void populateTakerPays(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfTakerPays, [&proto]() { return proto.mutable_taker_pays(); });
}


template <class T>
void populateDestination(STObject const& obj, T& proto)
{
    populateProtoAccount(obj, sfDestination, [&proto]() {
        return proto.mutable_destination();
    });
}


template <class T>
void populateCheckID(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfCheckID, [&proto]() { return proto.mutable_check_id(); });
}

template <class T>
void populateAmount(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfAmount, [&proto]() { return proto.mutable_amount(); });
}

template <class T>
void populateDeliverMin(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfDeliverMin, [&proto]() { return proto.mutable_deliver_min(); });
}

template <class T>
void populateSendMax(STObject const& obj, T& proto)
{

    populateProtoAmount(
        obj, sfSendMax, [&proto]() { return proto.mutable_send_max(); });
}

template <class T>
void populateDestinationTag(STObject const& obj, T& proto)
{

    populateProtoU32(obj, sfDestinationTag, [&proto]() {
        return proto.mutable_destination_tag();
    });
}

template <class T>
void populateInvoiceID(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfInvoiceID, [&proto]() { return proto.mutable_invoice_id(); });
}

template <class T>
void populateAuthorize(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfAuthorize, [&proto]() { return proto.mutable_authorize(); });
}

template <class T>
void populateUnauthorize(STObject const& obj, T& proto)
{
    populateProtoAccount(obj, sfUnauthorize, [&proto]() {
        return proto.mutable_unauthorize();
    });
}

template <class T>
void populateOwner(STObject const& obj, T& proto)
{

    populateProtoAccount(
        obj, sfOwner, [&proto]() { return proto.mutable_owner(); });
}

template <class T>
void populateCancelAfter(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfCancelAfter, [&proto]() {
        return proto.mutable_cancel_after();
    });
}

template <class T>
void populateFinishAfter(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfFinishAfter, [&proto]() {
        return proto.mutable_finish_after();
    });
}


template <class T>
void populateCondition(STObject const& obj, T& proto)
{

    populateProtoVL(
        obj, sfCondition, [&proto]() { return proto.mutable_condition(); });
}

template <class T>
void populateFulfillment(STObject const& obj, T& proto)
{
    populateProtoVL(obj, sfFulfillment, [&proto]() {
        return proto.mutable_fulfillment();
    });

}

template <class T>
void populateChannel(STObject const& obj, T& proto)
{
    populateProtoH256(
        obj, sfPayChannel, [&proto]() { return proto.mutable_channel(); });
}

template <class T>
void populateBalance(STObject const& obj, T& proto)
{
    populateProtoAmount(
        obj, sfBalance, [&proto]() { return proto.mutable_balance(); });
}

template <class T>
void populateSignature(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfSignature, [&proto]() { return proto.mutable_signature(); });
}

template <class T>
void populatePublicKey(STObject const& obj, T& proto)
{
    populateProtoVL(
        obj, sfPublicKey, [&proto]() { return proto.mutable_public_key(); });
}

template <class T>
void populateSettleDelay(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfSettleDelay, [&proto]() {
        return proto.mutable_settle_delay();
    });
}


template <class T>
void populateRegularKey(STObject const& obj, T& proto)
{
    populateProtoAccount(
        obj, sfRegularKey, [&proto]() { return proto.mutable_regular_key(); });
}

template <class T>
void populateSignerQuorum(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfSignerQuorum, [&proto]() {
        return proto.mutable_signer_quorum();
    });
}
template <class T>
void populateLimitAmount(STObject const& obj, T& proto)
{

    populateProtoAmount(obj, sfLimitAmount, [&proto]() {
        return proto.mutable_limit_amount();
    });
}
template <class T>
void populateQualityIn(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfQualityIn, [&proto]() { return proto.mutable_quality_in(); });
}

template <class T>
void populateQualityOut(STObject const& obj, T& proto)
{
    populateProtoU32(
        obj, sfQualityOut, [&proto]() { return proto.mutable_quality_out(); });
}

template <class T>
void populateAccount(STObject const& obj, T& proto)
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
        proto.mutable_fee()->set_drops(
            obj.getFieldAmount(sfFee).xrp().drops());
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
void populateFlags(STObject const& obj, T& proto)
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
void populateSourceTag(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfSourceTag, [&proto]() {
        return proto.mutable_source_tag();
    });
}

template <class T>
void populateAccountTransactionID(STObject const& obj, T& proto)
{
    populateProtoH256(obj, sfAccountTxnID, [&proto]() {
        return proto.mutable_account_transaction_id();
    });
}

template <class T>
void populateMemoData(STObject const& obj, T& proto)
{
    populateProtoVL(obj, sfMemoData, [&proto]() {
        return proto.mutable_memo_data();
    });
}





template <class T>
void populateMemoFormat(STObject const& obj, T& proto)
{
    populateProtoVL(obj, sfMemoFormat, [&proto]() {
        return proto.mutable_memo_format();
    });
}

template <class T>
void populateMemoType(STObject const& obj, T& proto)
{
    populateProtoVL(obj, sfMemoType, [&proto]() {
        return proto.mutable_memo_type();
    });
}

template <class T>
void populateSequence(STObject const& obj, T& proto)
{
    populateProtoU32(obj, sfSequence, [&proto]() {
        return proto.mutable_sequence();
    });
}

template <class T>
void
populateMemos(STObject const& obj, T& proto)
{
    if (obj.isFieldPresent(sfMemos))
    {
        auto memos = obj.getFieldArray(sfMemos);
        for (auto it = memos.begin(); it != memos.end(); ++it)
        {
            if (it->isFieldPresent(sfMemo))
            {
                rpc::v1::Memo* elt = proto.add_memos();
                auto memo = it->getField(sfMemo).downcast<STObject>();

                populateMemoData(memo, *elt);
                populateMemoFormat(memo, *elt);
                populateMemoType(memo, *elt);
            }
        }
    }
}

template <class T>
void
populateSigners(STObject const& obj, T& proto)
{
    if (obj.isFieldPresent(sfSigners))
    {
        auto signers = obj.getFieldArray(sfSigners);

        for (auto it = signers.begin(); it != signers.end(); ++it)
        {
            if (it->isFieldPresent(sfSigner))
            {
                rpc::v1::Signer* elt = proto.add_signers();
                auto signer = it->getField(sfSigner).downcast<STObject>();

                populateAccount(signer, *elt);
                populateTransactionSignature(signer, *elt);
                populateSigningPublicKey(signer, *elt);
            }
        }
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

#endif
