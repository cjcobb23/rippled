//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLED_GRPCTESTCLIENTBASE_H
#define RIPPLED_GRPCTESTCLIENTBASE_H

#include <rpc/v1/xrp_ledger.grpc.pb.h>

namespace ripple {

    struct GRPCTestClientBase
    {
        GRPCTestClientBase()
        : stub_(rpc::v1::XRPLedgerAPIService::NewStub(grpc::CreateChannel(
                "localhost:50051", grpc::InsecureChannelCredentials())))
        {}

        grpc::Status status;
        grpc::ClientContext context;
        std::unique_ptr<rpc::v1::XRPLedgerAPIService::Stub> stub_;
    };

    std::string textBlobToActualBlob(std::string const& blob)
    {
        std::string actual_blob;
        for(size_t i = 0; i < blob.size(); ++i)
        {
            unsigned int c;
            if(blob[i] >= 'A')
            {
                c = blob[i] - 'A' + 10;
            }
            else
            {
                c = blob[i] - '0';
            }
            c = c << 4;
            ++i;
            if(blob[i] >= 'A')
            {
                c += blob[i] - 'A' + 10;
            }
            else
            {
                c += blob[i] - '0';
            }
            actual_blob.push_back(c);
        }
        return actual_blob;
    }

    std::string actualBlobToTextBlob(std::string const& blob)
    {
        std::string text_blob;
        for(size_t i = 0; i < blob.size(); ++i)
        {
            int byte = blob[i];
            int low = byte & 0x0F;
            int hi = (byte & 0xF0) >> 4;

            char c1;
            if(hi >= 10)
            {
                c1 = 'A' + hi - 10;
            }
            else
            {
                c1 = '0' + hi;
            }
            text_blob.push_back(c1);
            char c2;
            if(low >= 10)
            {
                c2 = 'A' + low - 10;
            }
            else
            {
                c2 = '0' + low;
            }

            text_blob.push_back(c2);
        }
        return text_blob;
    }

} // ripple
#endif //RIPPLED_GRPCTESTCLIENTBASE_H
