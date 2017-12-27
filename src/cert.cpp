// Copyright (c) 2015-2017 The Syscoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "cert.h"
#include "alias.h"
#include "offer.h"
#include "init.h"
#include "validation.h"
#include "util.h"
#include "random.h"
#include "base58.h"
#include "core_io.h"
#include "rpc/server.h"
#include "wallet/wallet.h"
#include "chainparams.h"
#include "coincontrol.h"
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <mongoc.h>
using namespace std;
extern mongoc_collection_t *cert_collection;
extern mongoc_collection_t *certhistory_collection;
extern void SendMoneySyscoin(const vector<unsigned char> &vchAlias, const vector<unsigned char> &vchWitness, const CRecipient &aliasRecipient, CRecipient &aliasPaymentRecipient, vector<CRecipient> &vecSend, CWalletTx& wtxNew, CCoinControl* coinControl, bool fUseInstantSend=false, bool transferAlias=false);
bool IsCertOp(int op) {
    return op == OP_CERT_ACTIVATE
        || op == OP_CERT_UPDATE
        || op == OP_CERT_TRANSFER;
}

uint64_t GetCertExpiration(const CCert& cert) {
	uint64_t nTime = chainActive.Tip()->GetMedianTimePast() + 1;
	CAliasUnprunable aliasUnprunable;
	if (paliasdb && paliasdb->ReadAliasUnprunable(cert.aliasTuple.first, aliasUnprunable) && !aliasUnprunable.IsNull())
		nTime = aliasUnprunable.nExpireTime;
	
	return nTime;
}


string certFromOp(int op) {
    switch (op) {
    case OP_CERT_ACTIVATE:
        return "certactivate";
    case OP_CERT_UPDATE:
        return "certupdate";
    case OP_CERT_TRANSFER:
        return "certtransfer";
    default:
        return "<unknown cert op>";
    }
}
bool CCert::UnserializeFromData(const vector<unsigned char> &vchData, const vector<unsigned char> &vchHash) {
    try {
        CDataStream dsCert(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsCert >> *this;

		vector<unsigned char> vchCertData;
		Serialize(vchCertData);
		const uint256 &calculatedHash = Hash(vchCertData.begin(), vchCertData.end());
		const vector<unsigned char> &vchRandCert = vchFromValue(calculatedHash.GetHex());
		if(vchRandCert != vchHash)
		{
			SetNull();
			return false;
		}

    } catch (std::exception &e) {
		SetNull();
        return false;
    }
	return true;
}
bool CCert::UnserializeFromTx(const CTransaction &tx) {
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	int nOut;
	if(!GetSyscoinData(tx, vchData, vchHash, nOut))
	{
		SetNull();
		return false;
	}
	if(!UnserializeFromData(vchData, vchHash))
	{	
		return false;
	}
    return true;
}
void CCert::Serialize( vector<unsigned char> &vchData) {
    CDataStream dsCert(SER_NETWORK, PROTOCOL_VERSION);
    dsCert << *this;
	vchData = vector<unsigned char>(dsCert.begin(), dsCert.end());

}
void CCertDB::WriteCertIndex(const CCert& cert, const int& op) {
	if (!cert_collection)
		return;
	bson_error_t error;
	bson_t *update = NULL;
	bson_t *selector = NULL;
	mongoc_write_concern_t* write_concern = NULL;
	UniValue oName(UniValue::VOBJ);

	mongoc_update_flags_t update_flags;
	update_flags = (mongoc_update_flags_t)(MONGOC_UPDATE_NO_VALIDATE | MONGOC_UPDATE_UPSERT);
	selector = BCON_NEW("_id", BCON_UTF8(stringFromVch(cert.vchCert).c_str()));
	write_concern = mongoc_write_concern_new();
	mongoc_write_concern_set_w(write_concern, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
	if (BuildCertIndexerJson(cert, oName)) {
		update = bson_new_from_json((unsigned char *)oName.write().c_str(), -1, &error);
		if (!update || !mongoc_collection_update(cert_collection, update_flags, selector, update, write_concern, &error)) {
			LogPrintf("MONGODB CERT UPDATE ERROR: %s\n", error.message);
		}
	}
	if (update)
		bson_destroy(update);
	if (selector)
		bson_destroy(selector);
	if (write_concern)
		mongoc_write_concern_destroy(write_concern);
	WriteCertIndexHistory(cert, op);
}
void CCertDB::WriteCertIndexHistory(const CCert& cert, const int &op) {
	if (!certhistory_collection)
		return;
	bson_error_t error;
	bson_t *insert = NULL;
	mongoc_write_concern_t* write_concern = NULL;
	UniValue oName(UniValue::VOBJ);
	write_concern = mongoc_write_concern_new();
	mongoc_write_concern_set_w(write_concern, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);

	if (BuildCertIndexerHistoryJson(cert, oName)) {
		oName.push_back(Pair("op", certFromOp(op)));
		insert = bson_new_from_json((unsigned char *)oName.write().c_str(), -1, &error);
		if (!insert || !mongoc_collection_insert(certhistory_collection, (mongoc_insert_flags_t)MONGOC_INSERT_NO_VALIDATE, insert, write_concern, &error)) {
			LogPrintf("MONGODB CERT HISTORY ERROR: %s\n", error.message);
		}
	}

	if (insert)
		bson_destroy(insert);
	if (write_concern)
		mongoc_write_concern_destroy(write_concern);
}
	
void CCertDB::EraseCertIndexHistory(const std::vector<unsigned char>& vchCert, bool cleanup) {
	bson_error_t error;
	bson_t *selector = NULL;
	mongoc_write_concern_t* write_concern = NULL;
	mongoc_remove_flags_t remove_flags;
	remove_flags = (mongoc_remove_flags_t)(MONGOC_REMOVE_NONE);
	selector = BCON_NEW("cert", BCON_UTF8(stringFromVch(vchCert).c_str()));
	write_concern = mongoc_write_concern_new();
	mongoc_write_concern_set_w(write_concern, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
	if (!mongoc_collection_remove(certhistory_collection, remove_flags, selector, cleanup ? NULL : write_concern, &error)) {
		LogPrintf("MONGODB CERT HISTORY REMOVE ERROR: %s\n", error.message);
	}
	if (selector)
		bson_destroy(selector);
	if (write_concern)
		mongoc_write_concern_destroy(write_concern);
}
void CCertDB::EraseCertIndexHistory(const string& id) {
	bson_error_t error;
	bson_t *selector = NULL;
	mongoc_write_concern_t* write_concern = NULL;
	mongoc_remove_flags_t remove_flags;
	remove_flags = (mongoc_remove_flags_t)(MONGOC_REMOVE_NONE);
	selector = BCON_NEW("_id", BCON_UTF8(id.c_str()));
	write_concern = mongoc_write_concern_new();
	mongoc_write_concern_set_w(write_concern, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
	if (!mongoc_collection_remove(certhistory_collection, remove_flags, selector, write_concern, &error)) {
		LogPrintf("MONGODB CERT HISTORY REMOVE ERROR: %s\n", error.message);
	}
	if (selector)
		bson_destroy(selector);
	if (write_concern)
		mongoc_write_concern_destroy(write_concern);
}
void CCertDB::EraseCertIndex(const std::vector<unsigned char>& vchCert, bool cleanup) {
	if (!cert_collection)
		return;
	bson_error_t error;
	bson_t *selector = NULL;
	mongoc_write_concern_t* write_concern = NULL;
	mongoc_remove_flags_t remove_flags;
	remove_flags = (mongoc_remove_flags_t)(MONGOC_REMOVE_NONE);
	selector = BCON_NEW("_id", BCON_UTF8(stringFromVch(vchCert).c_str()));
	write_concern = mongoc_write_concern_new();
	mongoc_write_concern_set_w(write_concern, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
	if (!mongoc_collection_remove(cert_collection, remove_flags, selector, cleanup ? NULL : write_concern, &error)) {
		LogPrintf("MONGODB CERT REMOVE ERROR: %s\n", error.message);
	}
	
	if (selector)
		bson_destroy(selector);
	if (write_concern)
		mongoc_write_concern_destroy(write_concern);
	EraseCertIndexHistory(vchCert, cleanup);
}
bool CCertDB::CleanupDatabase(int &servicesCleaned)
{
	boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
	pcursor->SeekToFirst();
	CCert txPos;
	pair<string, CNameTXIDTuple > key;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
			if (pcursor->GetKey(key) && key.first == "certi") {
				const CNameTXIDTuple &certTuple = key.second;
  				if (!GetCert(certTuple.first, txPos) || chainActive.Tip()->GetMedianTimePast() >= GetCertExpiration(txPos))
				{
					servicesCleaned++;
					EraseCert(certTuple, true);
				} 
				
            }
            pcursor->Next();
        } catch (std::exception &e) {
            return error("%s() : deserialize error", __PRETTY_FUNCTION__);
        }
    }
	return true;
}

bool GetCert(const CNameTXIDTuple &certTuple,
	CCert& txPos) {
	if (!pcertdb || !pcertdb->ReadCert(certTuple, txPos))
		return false;
	return true;
}
bool GetCert(const vector<unsigned char> &vchCert,
        CCert& txPos) {
	uint256 txid;
	if (!pcertdb || !pcertdb->ReadCertLastTXID(vchCert, txid) )
		return false;
    if (!pcertdb->ReadCert(CNameTXIDTuple(vchCert, txid), txPos))
        return false;
    if (chainActive.Tip()->GetMedianTimePast() >= GetCertExpiration(txPos)) {
		txPos.SetNull();
        string cert = stringFromVch(vchCert);
        return false;
    }

    return true;
}
bool DecodeAndParseCertTx(const CTransaction& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch, char &type)
{
	CCert cert;
	bool decode = DecodeCertTx(tx, op, nOut, vvch);
	bool parse = cert.UnserializeFromTx(tx);
	if (decode&&parse) {
		type = CERT;
		return true;
	}
	return false;
}
bool DecodeCertTx(const CTransaction& tx, int& op, int& nOut,
        vector<vector<unsigned char> >& vvch) {
    bool found = false;


    // Strict check - bug disallowed
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& out = tx.vout[i];
        vector<vector<unsigned char> > vvchRead;
        if (DecodeCertScript(out.scriptPubKey, op, vvchRead)) {
            nOut = i; found = true; vvch = vvchRead;
            break;
        }
    }
    if (!found) vvch.clear();
    return found;
}


bool DecodeCertScript(const CScript& script, int& op,
        vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc) {
    opcodetype opcode;
	vvch.clear();
    if (!script.GetOp(pc, opcode)) return false;
    if (opcode < OP_1 || opcode > OP_16) return false;
    op = CScript::DecodeOP_N(opcode);
	if (op != OP_SYSCOIN_CERT)
		return false;
	if (!script.GetOp(pc, opcode))
		return false;
	if (opcode < OP_1 || opcode > OP_16)
		return false;
	op = CScript::DecodeOP_N(opcode);
	if (!IsCertOp(op))
		return false;

	bool found = false;
	for (;;) {
		vector<unsigned char> vch;
		if (!script.GetOp(pc, opcode, vch))
			return false;
		if (opcode == OP_DROP || opcode == OP_2DROP)
		{
			found = true;
			break;
		}
		if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
			return false;
		vvch.push_back(vch);
	}

	// move the pc to after any DROP or NOP
	while (opcode == OP_DROP || opcode == OP_2DROP) {
		if (!script.GetOp(pc, opcode))
			break;
	}

	pc--;
	return found;
}
bool DecodeCertScript(const CScript& script, int& op,
        vector<vector<unsigned char> > &vvch) {
    CScript::const_iterator pc = script.begin();
    return DecodeCertScript(script, op, vvch, pc);
}
bool RemoveCertScriptPrefix(const CScript& scriptIn, CScript& scriptOut) {
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeCertScript(scriptIn, op, vvch, pc))
		return false;
	scriptOut = CScript(pc, scriptIn.end());
	return true;
}

bool CheckCertInputs(const CTransaction &tx, int op, int nOut, const vector<vector<unsigned char> > &vvchArgs,
        bool fJustCheck, int nHeight, string &errorMessage, bool dontaddtodb) {
	if (tx.IsCoinBase() && !fJustCheck && !dontaddtodb)
	{
		LogPrintf("*Trying to add cert in coinbase transaction, skipping...");
		return true;
	}
	if (fDebug && !dontaddtodb)
		LogPrintf("*** CERT %d %d %s %s\n", nHeight,
			chainActive.Tip()->nHeight, tx.GetHash().ToString().c_str(),
			fJustCheck ? "JUSTCHECK" : "BLOCK");
	bool foundAlias = false;
	int prevAliasOp = 0;
    // Make sure cert outputs are not spent by a regular transaction, or the cert would be lost
	if (tx.nVersion != SYSCOIN_TX_VERSION) 
	{
		errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2000 - " + _("Non-Syscoin transaction found");
		return true;
	}
	vector<vector<unsigned char> > vvchPrevAliasArgs;
	// unserialize cert from txn, check for valid
	CCert theCert;
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	int nDataOut;
	if(!GetSyscoinData(tx, vchData, vchHash, nDataOut) || !theCert.UnserializeFromData(vchData, vchHash))
	{
		errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2001 - " + _("Cannot unserialize data inside of this transaction relating to a certificate");
		return true;
	}

	if(fJustCheck)
	{
		if(vvchArgs.size() != 2)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2002 - " + _("Certificate arguments incorrect size");
			return error(errorMessage.c_str());
		}

					
		if(vvchArgs.size() <= 1 || vchHash != vvchArgs[1])
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2003 - " + _("Hash provided doesn't match the calculated hash of the data");
			return true;
		}
			
		// Strict check - bug disallowed
		for (unsigned int i = 0; i < tx.vin.size(); i++) {
			vector<vector<unsigned char> > vvch;
			int pop;
			CCoins prevCoins;
			// ensure inputs are unspent when doing consensus check to add to block
			if (!GetUTXOCoins(tx.vin[i].prevout, prevCoins) || !IsSyscoinScript(prevCoins.vout[tx.vin[i].prevout.n].scriptPubKey, pop, vvch))
			{
				continue;
			}
			if(foundAlias)
				break;
			else if (!foundAlias && IsAliasOp(pop) && vvch.size() >= 2 && theCert.aliasTuple.first == vvch[0] && theCert.aliasTuple.third == vvch[1])
			{
				foundAlias = true; 
				prevAliasOp = pop;
				vvchPrevAliasArgs = vvch;
			}
		}
	}


	
	CAliasIndex alias;
	string retError = "";
	if(fJustCheck)
	{
		if (vvchArgs.empty() ||  vvchArgs[0].size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2004 - " + _("Certificate hex guid too long");
			return error(errorMessage.c_str());
		}
		if(theCert.sCategory.size() > MAX_NAME_LENGTH)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2005 - " + _("Certificate category too big");
			return error(errorMessage.c_str());
		}
		if(theCert.vchPubData.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2007 - " + _("Certificate public data too big");
			return error(errorMessage.c_str());
		}
		if(!theCert.vchCert.empty() && theCert.vchCert != vvchArgs[0])
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2008 - " + _("Guid in data output doesn't match guid in transaction");
			return error(errorMessage.c_str());
		}
		switch (op) {
		case OP_CERT_ACTIVATE:
			if (theCert.vchCert != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2009 - " + _("Certificate guid mismatch");
				return error(errorMessage.c_str());
			}
			if(!theCert.linkAliasTuple.first.empty())
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2010 - " + _("Certificate linked alias not allowed in activate");
				return error(errorMessage.c_str());
			}
			if(!IsAliasOp(prevAliasOp) || vvchPrevAliasArgs.empty())
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2011 - " + _("Alias input mismatch");
				return error(errorMessage.c_str());
			}
			if((theCert.vchTitle.size() > MAX_NAME_LENGTH || theCert.vchTitle.empty()))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2012 - " + _("Certificate title too big or is empty");
				return error(errorMessage.c_str());
			}
			if(!boost::algorithm::starts_with(stringFromVch(theCert.sCategory), "certificates"))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2013 - " + _("Must use a certificate category");
				return true;
			}
			break;

		case OP_CERT_UPDATE:
			if (theCert.vchCert != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2014 - " + _("Certificate guid mismatch");
				return error(errorMessage.c_str());
			}
			if(theCert.vchTitle.size() > MAX_NAME_LENGTH)
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2015 - " + _("Certificate title too big");
				return error(errorMessage.c_str());
			}
			if(!IsAliasOp(prevAliasOp) || vvchPrevAliasArgs.empty())
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2016 - " + _("Alias input mismatch");
				return error(errorMessage.c_str());
			}
			if(theCert.sCategory.size() > 0 && !boost::algorithm::istarts_with(stringFromVch(theCert.sCategory), "certificates"))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2017 - " + _("Must use a certificate category");
				return true;
			}
			break;

		case OP_CERT_TRANSFER:
			if (theCert.vchCert != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2018 - " + _("Certificate guid mismatch");
				return error(errorMessage.c_str());
			}
			if(!IsAliasOp(prevAliasOp) || vvchPrevAliasArgs.empty())
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2019 - " + _("Alias input mismatch");
				return error(errorMessage.c_str());
			}
			if(theCert.sCategory.size() > 0 && !boost::algorithm::istarts_with(stringFromVch(theCert.sCategory), "certificates"))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2020 - " + _("Must use a certificate category");
				return true;
			}
			break;

		default:
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2021 - " + _("Certificate transaction has unknown op");
			return error(errorMessage.c_str());
		}
	}

	// if not an certnew, load the cert data from the DB
	CCert dbCert;
	if (!GetCert(vvchArgs[0], dbCert))
	{
		if (op != OP_CERT_ACTIVATE) {
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2022 - " + _("Failed to read from certificate DB");
			return true;
		}
	}
	else
	{
		bool bInstantSendLocked = false;
		// if it was instant locked and this is a pow block (not instant send) then check to ensure that height >= stored height instead of < stored height
		// since instant send calls this function with chain height + 1
		if (!fJustCheck && pcertdb->ReadISLock(vvchArgs[0], bInstantSendLocked) && bInstantSendLocked) {
			if (dbCert.nHeight > nHeight)
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Certificate was already updated in this block.");
				return true;
			} 
			if (dbCert.txHash != tx.GetHash())
			{
				if (!dontaddtodb) {
					const string &txHashHex = dbCert.txHash.GetHex();
					pcertdb->EraseAliasIndexTxHistory(txHashHex);
					pcertdb->EraseCertIndexHistory(txHashHex);
				}
				if (op != OP_CERT_ACTIVATE && !pcertdb->ReadLastCert(vvchArgs[0], dbCert)) {
					errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 1048 - " + _("Failed to read last certificate from certificate DB");
					return true;
				}
				if (!dontaddtodb && !pcertdb->EraseISLock(vvchArgs[0]))
				{
					errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 1096 - " + _("Failed to erase Instant Send lock from certificate DB");
					return error(errorMessage.c_str());
				}
			}
			else {
				if (!dontaddtodb && !pcertdb->EraseISLock(vvchArgs[0]))
				{
					errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 1096 - " + _("Failed to erase Instant Send lock from certificate DB");
					return error(errorMessage.c_str());
				}
				return true;
			}
		}
		else if (dbCert.nHeight >= nHeight)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Certificate was already updated in this block.");
			return true;
		}
	}
	if(op != OP_CERT_ACTIVATE) 
	{
		if (dbCert.aliasTuple != theCert.aliasTuple)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Cannot update this certificate. Certificate owner must sign off on this change.");
			return true;
		}
		if(theCert.vchPubData.empty())
			theCert.vchPubData = dbCert.vchPubData;
		if(theCert.vchTitle.empty())
			theCert.vchTitle = dbCert.vchTitle;
		if(theCert.sCategory.empty())
			theCert.sCategory = dbCert.sCategory;

		uint256 txid;
		CCert firstCert;
		if (!pcertdb->ReadCertFirstTXID(dbCert.vchCert, txid)) {
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2025 - " + _("Cannot read first txid from cert DB");
			return true;
		}
		if (!GetCert(CNameTXIDTuple(dbCert.vchCert, txid), firstCert)) {
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2025 - " + _("Cannot read first cert from cert DB");
			return true;
		}
		if(op == OP_CERT_TRANSFER)
		{
			// check toalias
			if(!GetAlias(theCert.linkAliasTuple.first, alias))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2024 - " + _("Cannot find alias you are transferring to. It may be expired");	
				return true;
			}
			theCert.aliasTuple = theCert.linkAliasTuple;		
			if(!(alias.nAcceptTransferFlags & ACCEPT_TRANSFER_CERTIFICATES))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2025 - " + _("The alias you are transferring to does not accept certificate transfers");
				return true;
			}
				
			// the original owner can modify certificate regardless of access flags, new owners must adhere to access flags
			if(dbCert.nAccessFlags < 2 && dbCert.aliasTuple != firstCert.aliasTuple)
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Cannot transfer this certificate. Insufficient privileges.");
				return true;
			}
		}
		else if(op == OP_CERT_UPDATE)
		{
			if(dbCert.nAccessFlags < 1 && dbCert.aliasTuple != firstCert.aliasTuple)
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Cannot edit this certificate. It is view-only.");
				return true;
			}
		}
		if(theCert.nAccessFlags > dbCert.nAccessFlags && dbCert.aliasTuple != firstCert.aliasTuple)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Cannot modify for more lenient access. Only tighter access level can be granted.");
			return true;
		}
	}
	else
	{
		uint256 txid;
		if (pcertdb->ReadCertLastTXID(vvchArgs[0], txid))
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2027 - " + _("Certificate already exists");
			return true;
		}
	}
	if(!dontaddtodb) {
		string strResponseEnglish = "";
		string strResponseGUID = "";
		string strResponse = GetSyscoinTransactionDescription(op, vvchArgs, strResponseEnglish, strResponseGUID, CERT);
		if (strResponse != "") {
			const string &user1 = stringFromVch(vvchArgs[0]);
			string user2 = "";
			string user3 = "";
			if (op == OP_CERT_TRANSFER) {
				if (!theCert.linkAliasTuple.IsNull())
					user2 = stringFromVch(theCert.linkAliasTuple.first);
			}
			paliasdb->WriteAliasIndexTxHistory(user1, user2, user3, tx.GetHash(), nHeight, strResponseEnglish, strResponseGUID);
		}
	}
	theCert.linkAliasTuple.first.clear();
    // set the cert's txn-dependent values
	theCert.nHeight = nHeight;
	theCert.txHash = tx.GetHash();
    // write cert  
    if (!dontaddtodb && (!pcertdb->WriteCert(theCert, dbCert, op, fJustCheck) || (op == OP_CERT_ACTIVATE && !pcertdb->WriteCertFirstTXID(vvchArgs[0], theCert.txHash))))
	{
		errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2028 - " + _("Failed to write to certifcate DB");
        return error(errorMessage.c_str());
	}
		

      			
    // debug
	if(fDebug)
		LogPrintf( "CONNECTED CERT: op=%s cert=%s hash=%s height=%d fJustCheck=%d\n",
            certFromOp(op).c_str(),
            stringFromVch(vvchArgs[0]).c_str(),
            tx.GetHash().ToString().c_str(),
            nHeight,
			fJustCheck?1:0);

    return true;
}





UniValue certnew(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 5)
        throw runtime_error(
			"certnew [alias] [title] [public] [category=certificates] [witness]\n"
						"<alias> An alias you own.\n"
						"<title> title, 256 characters max.\n"
                        "<public> public data, 256 characters max.\n"
						"<category> category, 256 characters max. Defaults to certificates\n"
						"<witness> Witness alias name that will sign for web-of-trust notarization of this transaction.\n"
						+ HelpRequiringPassphrase());
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
    vector<unsigned char> vchTitle = vchFromString(params[1].get_str());
	vector<unsigned char> vchPubData = vchFromString(params[2].get_str());
	string strCategory = "certificates";
	strCategory = params[3].get_str();
	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[4]);

	// check for alias existence in DB
	CAliasIndex theAlias;

	if (!GetAlias(vchAlias, theAlias))
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2500 - " + _("failed to read alias from alias DB"));

	
    // gather inputs
	vector<unsigned char> vchCert = vchFromString(GenerateSyscoinGuid());
    // this is a syscoin transaction
    CWalletTx wtx;

    CScript scriptPubKeyOrig;
	CSyscoinAddress aliasAddress;
	GetAddress(theAlias, &aliasAddress, scriptPubKeyOrig);


    CScript scriptPubKey,scriptPubKeyAlias;

	// calculate net
    // build cert object
    CCert newCert;
	newCert.vchCert = vchCert;
	newCert.sCategory = vchFromString(strCategory);
	newCert.vchTitle = vchTitle;
	newCert.vchPubData = vchPubData;
	newCert.nHeight = chainActive.Tip()->nHeight;
	newCert.aliasTuple = CNameTXIDTuple(vchAlias, theAlias.txHash, theAlias.vchGUID);

	vector<unsigned char> data;
	newCert.Serialize(data);
    uint256 hash = Hash(data.begin(), data.end());
 	
    vector<unsigned char> vchHashCert = vchFromValue(hash.GetHex());

    scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_CERT) << CScript::EncodeOP_N(OP_CERT_ACTIVATE) << vchCert << vchHashCert << OP_2DROP << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << theAlias.vchAlias << theAlias.vchGUID << vchFromString("") << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;

	// use the script pub key to create the vecsend which sendmoney takes and puts it into vout
	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	CRecipient aliasPaymentRecipient;
	CreateAliasRecipient(scriptPubKeyOrig, aliasPaymentRecipient);
		
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	
	
	CCoinControl coinControl;
	coinControl.fAllowOtherInputs = false;
	coinControl.fAllowWatchOnly = false;	
	SendMoneySyscoin(vchAlias, vchWitness, aliasRecipient, aliasPaymentRecipient, vecSend, wtx, &coinControl);
	UniValue res(UniValue::VARR);
	res.push_back(EncodeHexTx(wtx));
	res.push_back(stringFromVch(vchCert));
	return res;
}

UniValue certupdate(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 5)
        throw runtime_error(
			"certupdate [guid] [title] [public] [category=certificates] [witness]\n"
						"Perform an update on an certificate you control.\n"
						"<guid> Certificate guidkey.\n"
						"<title> Certificate title, 256 characters max.\n"
                        "<public> Public data, 256 characters max.\n"                
						"<category> Category, 256 characters max. Defaults to certificates\n"
						"<witness> Witness alias name that will sign for web-of-trust notarization of this transaction.\n"
						+ HelpRequiringPassphrase());
	vector<unsigned char> vchCert = vchFromValue(params[0]);

	string strData = "";
	string strTitle = "";
	string strPubData = "";
	string strCategory = "";
	strTitle = params[1].get_str();
	strPubData = params[2].get_str();
	strCategory = params[3].get_str();

	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[4]);
    // this is a syscoind txn
    CWalletTx wtx;
    CScript scriptPubKeyOrig;
	CCert theCert;
	
    if (!GetCert( vchCert, theCert))
        throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2504 - " + _("Could not find a certificate with this key"));

	CAliasIndex theAlias;

	if (!GetAlias(theCert.aliasTuple.first, theAlias))
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2505 - " + _("Failed to read alias from alias DB"));

	CCert copyCert = theCert;
	theCert.ClearCert();
	CSyscoinAddress aliasAddress;
	GetAddress(theAlias, &aliasAddress, scriptPubKeyOrig);

    // create CERTUPDATE txn keys
    CScript scriptPubKey;

	if(strPubData != stringFromVch(theCert.vchPubData))
		theCert.vchPubData = vchFromString(strPubData);
	if(strCategory != stringFromVch(theCert.sCategory))
		theCert.sCategory = vchFromString(strCategory);
	if(strTitle != stringFromVch(theCert.vchTitle))
		theCert.vchTitle = vchFromString(strTitle);
	theCert.nHeight = chainActive.Tip()->nHeight;
	vector<unsigned char> data;
	theCert.Serialize(data);
    uint256 hash = Hash(data.begin(), data.end());
 	
    vector<unsigned char> vchHashCert = vchFromValue(hash.GetHex());
    scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_CERT) << CScript::EncodeOP_N(OP_CERT_UPDATE) << vchCert << vchHashCert << OP_2DROP << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << theAlias.vchAlias << theAlias.vchGUID << vchFromString("") << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	CRecipient aliasPaymentRecipient;
	CreateAliasRecipient(scriptPubKeyOrig, aliasPaymentRecipient);
		
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	
	
	CCoinControl coinControl;
	coinControl.fAllowOtherInputs = false;
	coinControl.fAllowWatchOnly = false;	
	SendMoneySyscoin(theAlias.vchAlias, vchWitness, aliasRecipient, aliasPaymentRecipient, vecSend, wtx, &coinControl);
 	UniValue res(UniValue::VARR);
	res.push_back(EncodeHexTx(wtx));
	return res;
}


UniValue certtransfer(const UniValue& params, bool fHelp) {
 if (fHelp || params.size() != 5)
        throw runtime_error(
			"certtransfer [guid] [alias] [public] [accessflags=2] [witness]\n"
						"Transfer a certificate you own to another alias.\n"
						"<guid> certificate guidkey.\n"
						"<alias> alias to transfer to.\n"
                        "<public> public data, 256 characters max.\n"	
						"<accessflags> Set new access flags for new owner for this certificate, 0 for read-only, 1 for edit, 2 for edit and transfer access. Default is 2.\n"
						"<witness> Witness alias name that will sign for web-of-trust notarization of this transaction.\n"	
						+ HelpRequiringPassphrase());

    // gather & validate inputs
	vector<unsigned char> vchCert = vchFromValue(params[0]);
	vector<unsigned char> vchAlias = vchFromValue(params[1]);

	string strPubData = "";
	int nAccessFlags = 2;
	strPubData = params[2].get_str();
	nAccessFlags = params[3].get_int();
	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[4]);

	// check for alias existence in DB
	CAliasIndex toAlias;
	if (!GetAlias(vchAlias, toAlias))
		throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2509 - " + _("Failed to read transfer alias from DB"));

    // this is a syscoin txn
    CWalletTx wtx;
    CScript scriptPubKeyOrig, scriptPubKeyFromOrig;

	CCert theCert;
    if (!GetCert( vchCert, theCert))
        throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2510 - " + _("Could not find a certificate with this key"));

	CAliasIndex fromAlias;
	if(!GetAlias(theCert.aliasTuple.first, fromAlias))
	{
		 throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2511 - " + _("Could not find the certificate alias"));
	}

	CSyscoinAddress sendAddr;
	GetAddress(toAlias, &sendAddr, scriptPubKeyOrig);
	CSyscoinAddress fromAddr;
	GetAddress(fromAlias, &fromAddr, scriptPubKeyFromOrig);

	CCert copyCert = theCert;
	theCert.ClearCert();
    CScript scriptPubKey;
	theCert.nHeight = chainActive.Tip()->nHeight;
	theCert.aliasTuple = CNameTXIDTuple(fromAlias.vchAlias, fromAlias.txHash, fromAlias.vchGUID);
	theCert.linkAliasTuple = CNameTXIDTuple(toAlias.vchAlias, toAlias.txHash, toAlias.vchGUID);

	if(strPubData != stringFromVch(theCert.vchPubData))
		theCert.vchPubData = vchFromString(strPubData);

	theCert.nAccessFlags = nAccessFlags;

	vector<unsigned char> data;
	theCert.Serialize(data);
    uint256 hash = Hash(data.begin(), data.end());
 	
    vector<unsigned char> vchHashCert = vchFromValue(hash.GetHex());
    scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_CERT) << CScript::EncodeOP_N(OP_CERT_TRANSFER) << vchCert << vchHashCert << OP_2DROP << OP_2DROP;
	scriptPubKey += scriptPubKeyOrig;
    // send the cert pay txn
	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);

	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << fromAlias.vchAlias << fromAlias.vchGUID << vchFromString("") << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyFromOrig;
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	CRecipient aliasPaymentRecipient;
	CreateAliasRecipient(scriptPubKeyFromOrig, aliasPaymentRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	
	
	CCoinControl coinControl;
	coinControl.fAllowOtherInputs = false;
	coinControl.fAllowWatchOnly = false;
	SendMoneySyscoin(fromAlias.vchAlias, vchWitness, aliasRecipient, aliasPaymentRecipient, vecSend, wtx, &coinControl);
	UniValue res(UniValue::VARR);
	res.push_back(EncodeHexTx(wtx));
	return res;
}


UniValue certinfo(const UniValue& params, bool fHelp) {
    if (fHelp || 1 > params.size())
        throw runtime_error("certinfo <guid>\n"
                "Show stored values of a single certificate and its .\n");

    vector<unsigned char> vchCert = vchFromValue(params[0]);
	UniValue oCert(UniValue::VOBJ);
    vector<unsigned char> vchValue;

	CCert txPos;
	uint256 txid;
	if (!pcertdb || !pcertdb->ReadCertLastTXID(vchCert, txid))
		throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 5535 - " + _("Failed to read from cert DB"));
	if (!GetCert(CNameTXIDTuple(vchCert, txid), txPos))
		throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 5535 - " + _("Failed to read from cert DB"));

	if(!BuildCertJson(txPos, oCert))
		oCert.clear();
    return oCert;
}
bool BuildCertJson(const CCert& cert, UniValue& oCert)
{
    oCert.push_back(Pair("_id", stringFromVch(cert.vchCert)));
    oCert.push_back(Pair("txid", cert.txHash.GetHex()));
    oCert.push_back(Pair("height", (int64_t)cert.nHeight));
	int64_t nTime = 0;
	if (chainActive.Height() >= cert.nHeight) {
		CBlockIndex *pindex = chainActive[cert.nHeight];
		if (pindex) {
			nTime = pindex->GetMedianTimePast();
		}
	}
	oCert.push_back(Pair("time", nTime));
	oCert.push_back(Pair("title", stringFromVch(cert.vchTitle)));
	oCert.push_back(Pair("publicvalue", stringFromVch(cert.vchPubData)));
	oCert.push_back(Pair("category", stringFromVch(cert.sCategory)));
	oCert.push_back(Pair("alias", stringFromVch(cert.aliasTuple.first)));
	oCert.push_back(Pair("access_flags", cert.nAccessFlags));
	int64_t expired_time = GetCertExpiration(cert);
	bool expired = false;
    if(expired_time <= chainActive.Tip()->GetMedianTimePast())
	{
		expired = true;
	}  


	oCert.push_back(Pair("expires_on", expired_time));
	oCert.push_back(Pair("expired", expired));
	return true;
}
bool BuildCertIndexerHistoryJson(const CCert& cert, UniValue& oCert)
{
	oCert.push_back(Pair("_id", cert.txHash.GetHex()));
	oCert.push_back(Pair("cert", stringFromVch(cert.vchCert)));
	oCert.push_back(Pair("height", (int64_t)cert.nHeight));
	int64_t nTime = 0;
	if (chainActive.Height() >= cert.nHeight) {
		CBlockIndex *pindex = chainActive[cert.nHeight];
		if (pindex) {
			nTime = pindex->GetMedianTimePast();
		}
	}
	oCert.push_back(Pair("time", nTime));
	oCert.push_back(Pair("title", stringFromVch(cert.vchTitle)));
	oCert.push_back(Pair("publicvalue", stringFromVch(cert.vchPubData)));
	oCert.push_back(Pair("category", stringFromVch(cert.sCategory)));
	oCert.push_back(Pair("alias", stringFromVch(cert.aliasTuple.first)));
	oCert.push_back(Pair("access_flags", cert.nAccessFlags));
	return true;
}
bool BuildCertIndexerJson(const CCert& cert, UniValue& oCert)
{
	oCert.push_back(Pair("_id", stringFromVch(cert.vchCert)));
	oCert.push_back(Pair("title", stringFromVch(cert.vchTitle)));
	oCert.push_back(Pair("height", (int)cert.nHeight));
	oCert.push_back(Pair("category", stringFromVch(cert.sCategory)));
	oCert.push_back(Pair("alias", stringFromVch(cert.aliasTuple.first)));
	return true;
}
void CertTxToJSON(const int op, const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash, UniValue &entry)
{
	string opName = certFromOp(op);
	CCert cert;
	if(!cert.UnserializeFromData(vchData, vchHash))
		return;

	CCert dbCert;
	GetCert(CNameTXIDTuple(cert.vchCert, cert.txHash), dbCert);
	

	entry.push_back(Pair("txtype", opName));
	entry.push_back(Pair("_id", stringFromVch(cert.vchCert)));

	if(!cert.vchTitle.empty() && cert.vchTitle != dbCert.vchTitle)
		entry.push_back(Pair("title", stringFromVch(cert.vchTitle)));

	if(!cert.vchPubData.empty() && cert.vchPubData != dbCert.vchPubData)
		entry.push_back(Pair("publicdata", stringFromVch(cert.vchPubData)));

	if(!cert.linkAliasTuple.first.empty() && cert.linkAliasTuple != dbCert.aliasTuple)
		entry.push_back(Pair("alias", stringFromVch(cert.linkAliasTuple.first)));
	else if(cert.aliasTuple.first != dbCert.aliasTuple.first)
		entry.push_back(Pair("alias", stringFromVch(cert.aliasTuple.first)));

	if(cert.nAccessFlags != dbCert.nAccessFlags)
		entry.push_back(Pair("access_flags", cert.nAccessFlags));




}




