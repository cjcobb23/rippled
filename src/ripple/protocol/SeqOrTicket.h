//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2018 Ripple Labs Inc.

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

#ifndef RIPPLE_PROTOCOL_SEQ_OR_TICKET_H_INCLUDED
#define RIPPLE_PROTOCOL_SEQ_OR_TICKET_H_INCLUDED

#include <cstdint>
#include <ostream>

namespace ripple {

/** A type that represents either a sequence value or a ticket value.

  We use the value() of a SeqOrTicket in places where a sequence was used
  before.  An example of this is the sequence of an Offer stored in the
  ledger.  We do the same thing with the in-ledger identifier of a
  Check, Payment Channel, and Escrow.

  Why is this safe?  If we use the SeqOrTicket::value(), how do we know that
  each ledger entry will be unique?

  There are two components that make this safe:

  1. When an account creates tickets it must use a sequence number.  You may
     not use a ticket to create tickets.

     Because the account used that sequence to create the ticket, we know that
     for the given account the sequence was consumed by the "TicketCreate"
     transaction.  Since a sequence can only be used once, we know that
     sequence cannot be used for an offer or any other transaction.  It was
     used by the "TicketCreate"

     Since the sequence number on the ticket was previously used to build a
     ticket, that same sequence number cannot have previously been used to
     create (for example) an Offer on the same account.  So as long as we make
     sure a ticket can only be consumed by one transaction, we can be
     confident that using the ticket number to fill in the sequence of an
     Offer would not duplicate a sequence used on another Offer created by
     the same account.

  2. When a "TicketCreate" transaction creates a batch of tickets it advances
     the account root sequence to one past the largest created ticket.

     Therefore all tickets in a batch other than the first may never have
     the same value as a sequence on that same account.  And since a ticket
     may only be used once there will never be any duplicates within this
     account.
*/
class SeqOrTicket
{
public:
    enum Type : std::uint8_t
    {
        seq = 0,
        ticket
    };

private:
    std::uint32_t value_;
    Type type_;

public:
    constexpr explicit SeqOrTicket (Type t, std::uint32_t v)
    : value_ {v}
    , type_ {t}
    { }

    SeqOrTicket (SeqOrTicket const& other) = default;

    SeqOrTicket& operator= (SeqOrTicket const& other) = default;

    constexpr std::uint32_t value() const
    {
        return value_;
    }

    constexpr bool isSeq() const
    {
        return type_ == seq;
    }

    constexpr bool isTicket() const
    {
        return type_ == ticket;
    }

    // Comparison
    //
    // The comparison is designed specifically so _all_ Sequence
    // representations sort in front of Ticket representations.  This
    // is true even if the Ticket value() is less that the Sequence
    // value().
    //
    // This somewhat surprising sort order has benefits for transaction
    // processing.  It guarantees that transactions creating Tickets are
    // sorted in from of transactions that consume Tickets.
    friend constexpr bool operator== (SeqOrTicket lhs, SeqOrTicket rhs)
    {
        if (lhs.type_ != rhs.type_)
            return false;
        return (lhs.value() == rhs.value());
    }

    friend constexpr bool operator!= (SeqOrTicket lhs, SeqOrTicket rhs)
    {
        return ! (lhs == rhs);
    }

    friend constexpr bool operator< (SeqOrTicket lhs, SeqOrTicket rhs)
    {
        if (lhs.type_ != rhs.type_)
            return lhs.type_ < rhs.type_;
        return lhs.value() < rhs.value();
    }

    friend constexpr bool operator> (SeqOrTicket lhs, SeqOrTicket rhs)
    {
        return rhs < lhs;
    }

    friend constexpr bool operator>= (SeqOrTicket lhs, SeqOrTicket rhs)
    {
        return ! (lhs < rhs);
    }

    friend constexpr bool operator<= (SeqOrTicket lhs, SeqOrTicket rhs)
    {
        return ! (lhs > rhs);
    }

    friend std::ostream& operator<< (std::ostream& os, SeqOrTicket seqOrT)
    {
        os << (seqOrT.isSeq() ? "sequence " : "ticket ");
        os << seqOrT.value();
        return os;
    }

};
} // ripple

#endif
