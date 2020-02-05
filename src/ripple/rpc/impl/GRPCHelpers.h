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


#include "org/xrpl/rpc/v1/ledger_objects.pb.h"
#include "org/xrpl/rpc/v1/meta.pb.h"
#include "org/xrpl/rpc/v1/account_info.pb.h"
#include "org/xrpl/rpc/v1/transaction.pb.h"

#include <ripple/ledger/TxMeta.h>
#include <ripple/protocol/Protocol.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/app/misc/TxQ.h>

#include <functional>

#ifndef RIPPLE_RPC_GRPCHELPERS_H_INCLUDED
#define RIPPLE_RPC_GRPCHELPERS_H_INCLUDED

namespace ripple {
namespace RPC {

void
populateMeta(org::xrpl::rpc::v1::Meta& proto, std::shared_ptr<TxMeta> txMeta);


void
populateQueueData(
    org::xrpl::rpc::v1::QueueData& proto,
    std::map<TxSeq, TxQ::AccountTxDetails const> const& txs);


void
populateTransaction(
    org::xrpl::rpc::v1::Transaction& proto,
    std::shared_ptr<STTx const> txnSt);

void
populateTransactionResultType(org::xrpl::rpc::v1::TransactionResult& proto, TER result);

void
populateAccountRoot(org::xrpl::rpc::v1::AccountRoot& proto, STObject const& obj);

void
populateSignerList(org::xrpl::rpc::v1::SignerList& proto, STObject const& obj);

void
populateAmount(org::xrpl::rpc::v1::CurrencyAmount& proto, STAmount const& amount);


template <class T>
void
populateProtoAmount(STAmount const& amount, T& proto)
{
    if (amount.native())
    {
        proto.mutable_value()->mutable_xrp_amount()->set_drops(
            amount.xrp().drops());
    }
    else
    {
        Issue const& issue = amount.issue();

        org::xrpl::rpc::v1::IssuedCurrencyAmount* issued =
            proto.mutable_value()->mutable_issued_currency_amount();

        issued->mutable_currency()->set_name(to_string(issue.currency));
        issued->mutable_currency()->set_code(
            issue.currency.data(), Currency::size());
        issued->mutable_issuer()->set_address(toBase58(issue.account));
        issued->set_value(to_string(amount.iou()));
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
