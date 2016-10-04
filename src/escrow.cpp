#include "escrow.h"
#include "offer.h"
#include "alias.h"
#include "cert.h"
#include "init.h"
#include "main.h"
#include "core_io.h"
#include "util.h"
#include "base58.h"
#include "rpc/server.h"
#include "wallet/wallet.h"
#include "policy/policy.h"
#include "script/script.h"
#include "chainparams.h"
#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/predicate.hpp>
using namespace std;
extern void SendMoney(const CTxDestination &address, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew);
extern void SendMoneySyscoin(const vector<CRecipient> &vecSend, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew, const CWalletTx* wtxInOffer=NULL, const CWalletTx* wtxInCert=NULL, const CWalletTx* wtxInAlias=NULL, const CWalletTx* wtxInEscrow=NULL, bool syscoinTx=true);
void PutToEscrowList(std::vector<CEscrow> &escrowList, CEscrow& index) {
	int i = escrowList.size() - 1;
	BOOST_REVERSE_FOREACH(CEscrow &o, escrowList) {
        if(index.nHeight != 0 && o.nHeight == index.nHeight) {
        	escrowList[i] = index;
            return;
        }
        else if(!o.txHash.IsNull() && o.txHash == index.txHash) {
        	escrowList[i] = index;
            return;
        }
        i--;
	}
    escrowList.push_back(index);
}
bool IsEscrowOp(int op) {
    return op == OP_ESCROW_ACTIVATE
        || op == OP_ESCROW_RELEASE
        || op == OP_ESCROW_REFUND
		|| op == OP_ESCROW_COMPLETE;
}
// % fee on escrow value for arbiter
int64_t GetEscrowArbiterFee(int64_t escrowValue, float fEscrowFee) {

	if(fEscrowFee == 0)
		fEscrowFee = 0.005;
	int fee = 1/fEscrowFee;
	int64_t nFee = escrowValue/fee;
	if(nFee < DEFAULT_MIN_RELAY_TX_FEE)
		nFee = DEFAULT_MIN_RELAY_TX_FEE;
	return nFee;
}
int GetEscrowExpirationDepth() {
	#ifdef ENABLE_DEBUGRPC
    return 1440;
  #else
    return 525600;
  #endif
}


string escrowFromOp(int op) {
    switch (op) {
    case OP_ESCROW_ACTIVATE:
        return "escrowactivate";
    case OP_ESCROW_RELEASE:
        return "escrowrelease";
    case OP_ESCROW_REFUND:
        return "escrowrefund";
	case OP_ESCROW_COMPLETE:
		return "escrowcomplete";
    default:
        return "<unknown escrow op>";
    }
}
bool CEscrow::UnserializeFromData(const vector<unsigned char> &vchData, const vector<unsigned char> &vchHash) {
    try {
        CDataStream dsEscrow(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsEscrow >> *this;

		const vector<unsigned char> &vchEscrowData = Serialize();
		uint256 calculatedHash = Hash(vchEscrowData.begin(), vchEscrowData.end());
		vector<unsigned char> vchRand = CScriptNum(calculatedHash.GetCheapHash()).getvch();
		vector<unsigned char> vchRandEscrow = vchFromValue(HexStr(vchRand));
		if(vchRandEscrow != vchHash)
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
bool CEscrow::UnserializeFromTx(const CTransaction &tx) {
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
const vector<unsigned char> CEscrow::Serialize() {
    CDataStream dsEscrow(SER_NETWORK, PROTOCOL_VERSION);
    dsEscrow << *this;
    const vector<unsigned char> vchData(dsEscrow.begin(), dsEscrow.end());
    return vchData;

}
bool CEscrowDB::ScanEscrows(const std::vector<unsigned char>& vchEscrow, const string& strRegexp, unsigned int nMax,
        std::vector<std::pair<std::vector<unsigned char>, CEscrow> >& escrowScan) {
	string strSearchLower = strRegexp;
	boost::algorithm::to_lower(strSearchLower);
	int nMaxAge  = GetEscrowExpirationDepth();
	boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
	pcursor->Seek(make_pair(string("escrowi"), vchEscrow));
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
		pair<string, vector<unsigned char> > key;
        try {
			if (pcursor->GetKey(key) && key.first == "escrowi") {
            	vector<unsigned char> vchEscrow = key.second;
                vector<CEscrow> vtxPos;
				pcursor->GetValue(vtxPos);
				if (vtxPos.empty()){
					pcursor->Next();
					continue;
				}
				const CEscrow &txPos = vtxPos.back();
  				if (chainActive.Tip()->nHeight - txPos.nHeight >= nMaxAge && txPos.op == OP_ESCROW_COMPLETE)
				{
					pcursor->Next();
					continue;
				}   
				const string &escrow = stringFromVch(vchEscrow);
				const string &offerstr = stringFromVch(txPos.vchOffer);

				string buyerAliasLower = stringFromVch(txPos.vchBuyerAlias);
				string sellerAliasLower = stringFromVch(txPos.vchSellerAlias);
				string arbiterAliasLower = stringFromVch(txPos.vchArbiterAlias);

				if (strRegexp != "" && strRegexp != offerstr && strRegexp != escrow && strSearchLower != buyerAliasLower && strSearchLower != sellerAliasLower && strSearchLower != arbiterAliasLower)
				{
					pcursor->Next();
					continue;
				}  
                escrowScan.push_back(make_pair(vchEscrow, txPos));
            }
            if (escrowScan.size() >= nMax)
                break;

            pcursor->Next();
        } catch (std::exception &e) {
            return error("%s() : deserialize error", __PRETTY_FUNCTION__);
        }
    }
    return true;
}
int IndexOfEscrowOutput(const CTransaction& tx) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return -1;
    vector<vector<unsigned char> > vvch;
	int op;
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		// find an output you own
		if (pwalletMain->IsMine(out) && DecodeEscrowScript(out.scriptPubKey, op, vvch)) {
			return i;
		}
	}
	return -1;
}
bool GetTxOfEscrow(const vector<unsigned char> &vchEscrow,
        CEscrow& txPos, CTransaction& tx) {
    vector<CEscrow> vtxPos;
    if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
        return false;
    txPos = vtxPos.back();
    int nHeight = txPos.nHeight;
	// if escrow is refunded or claimed and its expired
	// if not refunded or claimed it cannot expire
    if ((nHeight + GetEscrowExpirationDepth()
            < chainActive.Tip()->nHeight) && txPos.op == OP_ESCROW_COMPLETE) {
        string escrow = stringFromVch(vchEscrow);
        LogPrintf("GetTxOfEscrow(%s) : expired", escrow.c_str());
        return false;
    }
    if (!GetSyscoinTransaction(nHeight, txPos.txHash, tx, Params().GetConsensus()))
        return error("GetTxOfEscrow() : could not read tx from disk");

    return true;
}
bool GetTxAndVtxOfEscrow(const vector<unsigned char> &vchEscrow,
        CEscrow& txPos, CTransaction& tx, vector<CEscrow> &vtxPos) {
    
    if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
        return false;
    txPos = vtxPos.back();
    int nHeight = txPos.nHeight;
	// if escrow is refunded or claimed and its expired
	// if not refunded or claimed it cannot expire
    if ((nHeight + GetEscrowExpirationDepth()
            < chainActive.Tip()->nHeight) && txPos.op == OP_ESCROW_COMPLETE) {
        string escrow = stringFromVch(vchEscrow);
        LogPrintf("GetTxOfEscrow(%s) : expired", escrow.c_str());
        return false;
    }
    if (!GetSyscoinTransaction(nHeight, txPos.txHash, tx, Params().GetConsensus()))
        return error("GetTxOfEscrow() : could not read tx from disk");

    return true;
}

bool DecodeAndParseEscrowTx(const CTransaction& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch)
{
	CEscrow escrow;
	bool decode = DecodeEscrowTx(tx, op, nOut, vvch);
	bool parse = escrow.UnserializeFromTx(tx);
	return decode && parse;
}
bool DecodeEscrowTx(const CTransaction& tx, int& op, int& nOut,
        vector<vector<unsigned char> >& vvch) {
    bool found = false;


    // Strict check - bug disallowed
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& out = tx.vout[i];
        vector<vector<unsigned char> > vvchRead;
        if (DecodeEscrowScript(out.scriptPubKey, op, vvchRead)) {
            nOut = i; found = true; vvch = vvchRead;
            break;
        }
    }
	if (!found) vvch.clear();
    return found;
}

bool DecodeEscrowScript(const CScript& script, int& op,
        vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc) {
    opcodetype opcode;
	vvch.clear();
	if (!script.GetOp(pc, opcode)) return false;
	if (opcode < OP_1 || opcode > OP_16) return false;
    op = CScript::DecodeOP_N(opcode);
    for (;;) {
        vector<unsigned char> vch;
        if (!script.GetOp(pc, opcode, vch))
            return false;

        if (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
            break;
        if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
            return false;
        vvch.push_back(vch);
    }

    // move the pc to after any DROP or NOP
    while (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP) {
        if (!script.GetOp(pc, opcode))
            break;
    }
	
    pc--;
    return IsEscrowOp(op);
}
bool DecodeEscrowScript(const CScript& script, int& op,
        vector<vector<unsigned char> > &vvch) {
    CScript::const_iterator pc = script.begin();
    return DecodeEscrowScript(script, op, vvch, pc);
}

CScript RemoveEscrowScriptPrefix(const CScript& scriptIn) {
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeEscrowScript(scriptIn, op, vvch, pc))
	{
        throw runtime_error("RemoveEscrowScriptPrefix() : could not decode escrow script");
	}
	
    return CScript(pc, scriptIn.end());
}
bool CheckEscrowInputs(const CTransaction &tx, int op, int nOut, const vector<vector<unsigned char> > &vvchArgs, const CCoinsViewCache &inputs, bool fJustCheck, int nHeight, string &errorMessage, const CBlock* block, bool dontaddtodb) {
	if(!IsSys21Fork(nHeight))
		return true;	
	if (tx.IsCoinBase())
		return true;
	const COutPoint *prevOutput = NULL;
	CCoins prevCoins;
	int prevOp = 0;
	int prevAliasOp = 0;
	bool foundEscrow = false;
	bool foundAlias = false;
	if (fDebug)
		LogPrintf("*** ESCROW %d %d %s %s\n", nHeight,
			chainActive.Tip()->nHeight, tx.GetHash().ToString().c_str(),
			fJustCheck ? "JUSTCHECK" : "BLOCK");

    // Make sure escrow outputs are not spent by a regular transaction, or the escrow would be lost
    if (tx.nVersion != SYSCOIN_TX_VERSION)
	{
		errorMessage = "SYSCOIN_ESCROW_MESSAGE_ERROR: ERRCODE: 4000 - " + _("Non-Syscoin transaction found");
		return true;
	}
	 // unserialize escrow UniValue from txn, check for valid
    CEscrow theEscrow;
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	int nDataOut;
	if(GetSyscoinData(tx, vchData, vchHash, nDataOut) && !theEscrow.UnserializeFromData(vchData, vchHash))
	{
		theEscrow.SetNull();
	}
	if(theEscrow.IsNull())
	{
		if(fDebug)
			LogPrintf("SYSCOIN_ESCROW_CONSENSUS_ERROR: Null escrow, skipping...\n");	
		return true;
	}	
	vector<vector<unsigned char> > vvchPrevArgs, vvchPrevAliasArgs;
	if(fJustCheck)
	{
		if(!vchData.empty())
		{
			CRecipient fee;
			CScript scriptData;
			scriptData << vchData;
			CreateFeeRecipient(scriptData, vchData, fee);
			if (fee.nAmount > tx.vout[nDataOut].nValue) 
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4001 - " + _("Transaction does not pay enough fees");
				return error(errorMessage.c_str());
			}
		}			
		if(vvchArgs.size() != 3)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4002 - " + _("Escrow arguments incorrect size");
			return error(errorMessage.c_str());
		}
		if(!theEscrow.IsNull())
		{
			if(vvchArgs.size() <= 2 || vchHash != vvchArgs[2])
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4003 - " + _("Hash provided doesn't match the calculated hash the data");
				return error(errorMessage.c_str());
			}
		}


		// Strict check - bug disallowed
		for (unsigned int i = 0; i < tx.vin.size(); i++) {
			vector<vector<unsigned char> > vvch;
			int pop;
			prevOutput = &tx.vin[i].prevout;	
			if(!prevOutput)
				continue;
			// ensure inputs are unspent when doing consensus check to add to block
			if(!inputs.GetCoins(prevOutput->hash, prevCoins))
				continue;
			if(prevCoins.vout.size() <= prevOutput->n || !IsSyscoinScript(prevCoins.vout[prevOutput->n].scriptPubKey, pop, vvch))
				continue;
			if(foundEscrow && foundAlias)
				break;

			if (!foundEscrow && IsEscrowOp(pop)) {
				foundEscrow = true; 
				prevOp = pop;
				vvchPrevArgs = vvch;
			}
			else if (!foundAlias && IsAliasOp(pop))
			{
				foundAlias = true; 
				prevAliasOp = pop;
				vvchPrevAliasArgs = vvch;
			}
		}
	}

	vector<COffer> myVtxPos;
	CAliasIndex alias;
	CTransaction aliasTx;
    COffer theOffer;
	string retError = "";
	CTransaction txOffer;
	int escrowOp = OP_ESCROW_ACTIVATE;
	COffer dbOffer;
	if(fJustCheck)
	{
		if (vvchArgs.empty() || vvchArgs[0].size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4004 - " + _("Escrow guid too big");
			return error(errorMessage.c_str());
		}
		if(theEscrow.vchRedeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4005 - " + _("Escrow redeem script too long");
			return error(errorMessage.c_str());
		}
		if(theEscrow.feedback.size() > 0 && theEscrow.feedback[0].vchFeedback.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4006 - " + _("Feedback too long");
			return error(errorMessage.c_str());
		}
		if(theEscrow.feedback.size() > 1 && theEscrow.feedback[1].vchFeedback.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4007 - " + _("Feedback too long");
			return error(errorMessage.c_str());
		}
		if(theEscrow.vchOffer.size() > MAX_ID_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4008 - " + _("Escrow offer guid too long");
			return error(errorMessage.c_str());
		}
		if(!theEscrow.vchEscrow.empty() && theEscrow.vchEscrow != vvchArgs[0])
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4009 - " + _("Escrow guid in data output doesn't match guid in transaction");
			return error(errorMessage.c_str());
		}
		switch (op) {
			case OP_ESCROW_ACTIVATE:
				if(theEscrow.op != OP_ESCROW_ACTIVATE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4010 - " + _("Invalid op, should be escrow activate");
					return error(errorMessage.c_str());
				}
				if (theEscrow.vchEscrow != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4011 - " + _("Escrow Guid mismatch");
					return error(errorMessage.c_str());
				}
				if ((theEscrow.escrowInputTx.empty() && !theEscrow.txBTCId.IsNull()) || (theEscrow.txBTCId.IsNull() && !theEscrow.escrowInputTx.empty()))
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4012 - " + _("Not enough information to process BTC escrow payment");
					return error(errorMessage.c_str());
				}
				if(!theEscrow.feedback.empty())
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4013 - " + _("Cannot leave feedback in escrow activation");
					return error(errorMessage.c_str());
				}
				if(IsAliasOp(prevAliasOp) && vvchPrevAliasArgs[0] != theEscrow.vchBuyerAlias)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4014 - " + _("Whitelist guid mismatch");
					return error(errorMessage.c_str());
				}
				break;
			case OP_ESCROW_RELEASE:
				if(!IsAliasOp(prevAliasOp) || theEscrow.vchLinkAlias != vvchPrevAliasArgs[0] )
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4015 - " + _("Alias input mismatch");
					return error(errorMessage.c_str());
				}
				if(prevOp == OP_ESCROW_COMPLETE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4016 - " + _("Can only release an active escrow");
					return error(errorMessage.c_str());
				}	
				// Check input
				if (vvchPrevArgs.empty() || vvchArgs.empty() || vvchPrevArgs[0] != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4017 - " + _("Escrow input guid mismatch");
					return error(errorMessage.c_str());
				}
				if (vvchArgs.size() <= 1 || vvchArgs[1].size() > 1)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4018 - " + _("Escrow release status too large");
					return error(errorMessage.c_str());
				}
				if(!theEscrow.feedback.empty())
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4019 - " + _("Cannot leave feedback in escrow release");
					return error(errorMessage.c_str());
				}
				if(vvchArgs[1] == vchFromString("1"))
				{
					if(prevOp != OP_ESCROW_RELEASE || vvchPrevArgs[1] != vchFromString("0"))
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4020 - " + _("Can only claim a released escrow");
						return error(errorMessage.c_str());
					}
					if(theEscrow.op != OP_ESCROW_COMPLETE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4021 - " + _("Invalid op, should be escrow complete");
						return error(errorMessage.c_str());
					}

				}
				else
				{
					if(prevOp == OP_ESCROW_COMPLETE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4022 - " + _("Can only release an active escrow");
						return error(errorMessage.c_str());
					}
					if(theEscrow.op != OP_ESCROW_RELEASE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4023 - " + _("Invalid op, should be escrow release");
						return error(errorMessage.c_str());
					}
				}

				break;
			case OP_ESCROW_COMPLETE:
				if(!IsAliasOp(prevAliasOp) || theEscrow.vchLinkAlias != vvchPrevAliasArgs[0] )
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4024 - " + _("Alias input mismatch");
					return error(errorMessage.c_str());
				}
				
				if (theEscrow.op != OP_ESCROW_COMPLETE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4025 - " + _("Invalid op, should be escrow complete");
					return error(errorMessage.c_str());
				}
				if(theEscrow.feedback.empty())
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4026 - " + _("Feedback must leave a message");
					return error(errorMessage.c_str());
				}
						
				if(theEscrow.op != OP_ESCROW_COMPLETE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4027 - " + _("Invalid op, should be escrow complete");
					return error(errorMessage.c_str());
				}
				break;			
			case OP_ESCROW_REFUND:
				if(!IsAliasOp(prevAliasOp) || theEscrow.vchLinkAlias != vvchPrevAliasArgs[0] )
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4028 - " + _("Alias input missing");
					return error(errorMessage.c_str());
				}
				if (vvchArgs.size() <= 1 || vvchArgs[1].size() > 1)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4029 - " + _("Escrow refund status too large");
					return error(errorMessage.c_str());
				}
				if (vvchPrevArgs.empty() || vvchArgs.empty() || vvchPrevArgs[0] != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4030 - " + _("Escrow input guid mismatch");
					return error(errorMessage.c_str());
				}
				if (theEscrow.vchEscrow != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4031 - " + _("Guid mismatch");
					return error(errorMessage.c_str());
				}
				if(vvchArgs[1] == vchFromString("1"))
				{
					if(prevOp != OP_ESCROW_REFUND || vvchPrevArgs[1] != vchFromString("0"))
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4032 - " + _("Can only claim a refunded escrow");
						return error(errorMessage.c_str());
					}
					if(theEscrow.op != OP_ESCROW_COMPLETE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4033 - " + _("Invalid op, should be escrow complete");
						return error(errorMessage.c_str());
					}

				}
				else
				{
					if(prevOp == OP_ESCROW_COMPLETE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4034 - " + _("Can only refund an active escrow");
						return error(errorMessage.c_str());
					}
					if(theEscrow.op != OP_ESCROW_REFUND)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4035 - " + _("Invalid op, should be escrow refund");
						return error(errorMessage.c_str());
					}
				}
				// Check input
				if(!theEscrow.feedback.empty())
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4036 - " + _("Cannot leave feedback in escrow refund");
					return error(errorMessage.c_str());
				}
				


				break;
			default:
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4037 - " + _("Escrow transaction has unknown op");
				return error(errorMessage.c_str());
		}
	}



    if (!fJustCheck ) {
		if(op == OP_ESCROW_ACTIVATE) 
		{
			if(!GetTxOfAlias(theEscrow.vchBuyerAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4038 - " + _("Cannot find buyer alias. It may be expired");
				return true;
			}
			if(!GetTxOfAlias(theEscrow.vchSellerAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4039 - " + _("Cannot find seller alias. It may be expired");
				return true;
			}	
			if(!GetTxOfAlias(theEscrow.vchArbiterAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4040 - " + _("Cannot find arbiter alias. It may be expired");
				return true;
			}
		}
		vector<CEscrow> vtxPos;
		// make sure escrow settings don't change (besides rawTx) outside of activation
		if(op != OP_ESCROW_ACTIVATE) 
		{
			// save serialized escrow for later use
			CEscrow serializedEscrow = theEscrow;
			CTransaction escrowTx;
			if(!GetTxAndVtxOfEscrow(vvchArgs[0], theEscrow, escrowTx, vtxPos))	
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4041 - " + _("Failed to read from escrow DB");
				return true;
			}
			
			// make sure we have found this escrow in db
			if(!vtxPos.empty())
			{
				if (theEscrow.vchEscrow != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4042 - " + _("Escrow Guid mismatch");
					return true;
				}

				// these are the only settings allowed to change outside of activate
				if(!serializedEscrow.rawTx.empty())
					theEscrow.rawTx = serializedEscrow.rawTx;
				escrowOp = serializedEscrow.op;
				if(op == OP_ESCROW_REFUND && vvchArgs[1] == vchFromString("0"))
				{
					if(theEscrow.op == OP_ESCROW_COMPLETE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4043 - " + _("Can only refund an active escrow");
						return true;
					}
					else if(theEscrow.op == OP_ESCROW_RELEASE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4044 - " + _("Cannot refund an escrow that is already released");
						return true;
					}
					else if(serializedEscrow.vchLinkAlias != theEscrow.vchSellerAlias && serializedEscrow.vchLinkAlias != theEscrow.vchArbiterAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4045 - " + _("Only arbiter or seller can initiate an escrow refund");
						return true;
					}
					// only the arbiter can re-refund an escrow
					else if(theEscrow.op == OP_ESCROW_REFUND && serializedEscrow.vchLinkAlias != theEscrow.vchArbiterAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4046 - " + _("Only arbiter can refund an escrow after it has already been refunded");
						return true;
					}
					// make sure offer is still valid and then refund qty
					if (GetTxAndVtxOfOffer( theEscrow.vchOffer, dbOffer, txOffer, myVtxPos))
					{
						if(dbOffer.nQty != -1)
						{
							vector<COffer> myLinkVtxPos;
							unsigned int nQty = dbOffer.nQty + theEscrow.nQty;
							// if this is a linked offer we must update the linked offer qty aswell
							if (pofferdb->ExistsOffer(dbOffer.vchLinkOffer)) {
								if (pofferdb->ReadOffer(dbOffer.vchLinkOffer, myLinkVtxPos))
								{
									COffer myLinkOffer = myLinkVtxPos.back();
									myLinkOffer.nQty += theEscrow.nQty;
									if(myLinkOffer.nQty < 0)
										myLinkOffer.nQty = 0;
									nQty = myLinkOffer.nQty;
									myLinkOffer.PutToOfferList(myLinkVtxPos);
									if (!dontaddtodb && !pofferdb->WriteOffer(dbOffer.vchLinkOffer, myLinkVtxPos))
									{
										errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4047 - " + _("Failed to write to offer link to DB");
										return error(errorMessage.c_str());
									}					
									// go through the linked offers, if any, and update the linked offer qty based on the this qty
									for(unsigned int i=0;i<myLinkOffer.offerLinks.size();i++) {
										vector<COffer> myVtxPos;	
										if (pofferdb->ExistsOffer(myLinkOffer.offerLinks[i]) && myLinkOffer.offerLinks[i] != theEscrow.vchOffer) {
											if (pofferdb->ReadOffer(myLinkOffer.offerLinks[i], myVtxPos))
											{
												COffer offerLink = myVtxPos.back();					
												offerLink.nQty = nQty;	
												offerLink.PutToOfferList(myVtxPos);
												if (!dontaddtodb && !pofferdb->WriteOffer(myLinkOffer.offerLinks[i], myVtxPos))
												{
													errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4048 - " + _("Failed to write to offer link to DB");		
													return error(errorMessage.c_str());
												}
											}
										}
									}
								}
							}
							// go through the linked offers, if any, and update the linked offer qty based on the this qty
							for(unsigned int i=0;i<dbOffer.offerLinks.size();i++) {
								vector<COffer> myVtxPos;	
								if (pofferdb->ExistsOffer(dbOffer.offerLinks[i])) {
									if (pofferdb->ReadOffer(dbOffer.offerLinks[i], myVtxPos))
									{
										COffer offerLink = myVtxPos.back();					
										offerLink.nQty = nQty;	
										offerLink.PutToOfferList(myVtxPos);
										if (!dontaddtodb && !pofferdb->WriteOffer(dbOffer.offerLinks[i], myVtxPos))
										{
											errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4049 - " + _("Failed to write to offer link to DB");		
											return error(errorMessage.c_str());
										}
									}
								}
							}
							dbOffer.nQty = nQty;
							if(dbOffer.nQty < 0)
								dbOffer.nQty = 0;
							dbOffer.PutToOfferList(myVtxPos);
							if (!dontaddtodb && !pofferdb->WriteOffer(theEscrow.vchOffer, myVtxPos))
							{
								errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4050 - " + _("Failed to write to offer to DB");
								return error(errorMessage.c_str());
							}
						}			
					}
				}
				else if(op == OP_ESCROW_REFUND && vvchArgs[1] == vchFromString("1"))
				{
					if(!serializedEscrow.redeemTxId.IsNull())
						theEscrow.redeemTxId = serializedEscrow.redeemTxId;
					else if(serializedEscrow.vchLinkAlias != theEscrow.vchBuyerAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4051 - " + _("Only buyer can claim an escrow refund");
						return true;
					}
				}
				else if(op == OP_ESCROW_RELEASE && vvchArgs[1] == vchFromString("0"))
				{
					if(theEscrow.op == OP_ESCROW_COMPLETE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4052 - " + _("Can only release an active escrow");
						return true;
					}
					else if(theEscrow.op == OP_ESCROW_REFUND)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4053 - " + _("Cannot release an escrow that is already refunded");
						return true;
					}
					else if(serializedEscrow.vchLinkAlias != theEscrow.vchBuyerAlias && serializedEscrow.vchLinkAlias != theEscrow.vchArbiterAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4054 - " + _("Only arbiter or buyer can initiate an escrow release");
						return true;
					}
					// only the arbiter can re-release an escrow
					else if(theEscrow.op == OP_ESCROW_RELEASE && serializedEscrow.vchLinkAlias != theEscrow.vchArbiterAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4055 - " + _("Only arbiter can release an escrow after it has already been released");
						return true;
					}
				}
				else if(op == OP_ESCROW_RELEASE && vvchArgs[1] == vchFromString("1"))
				{
					if(!serializedEscrow.redeemTxId.IsNull())
						theEscrow.redeemTxId = serializedEscrow.redeemTxId;
					else if(serializedEscrow.vchLinkAlias != theEscrow.vchSellerAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4056 - " + _("Only seller can claim an escrow release");
						return true;
					}
				}
				else if(op == OP_ESCROW_COMPLETE)
				{	
					if(serializedEscrow.feedback.size() != 2)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4057 - " + _("Invalid number of escrow feedbacks provided");
						return true;
					}
					if(serializedEscrow.feedback[0].nFeedbackUserFrom ==  serializedEscrow.feedback[0].nFeedbackUserTo ||
						serializedEscrow.feedback[1].nFeedbackUserFrom ==  serializedEscrow.feedback[1].nFeedbackUserTo)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4058 - " + _("Cannot send yourself feedback");
						return true;
					}
					else if(serializedEscrow.feedback[0].vchFeedback.size() <= 0 && serializedEscrow.feedback[1].vchFeedback.size() <= 0)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4059 - " + _("Feedback must leave a message");
						return true;
					}
					else if(serializedEscrow.feedback[0].nRating > 5 || serializedEscrow.feedback[1].nRating > 5)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4060 - " + _("Invalid rating, must be less than or equal to 5 and greater than or equal to 0");
						return true;
					}
					else if((serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKBUYER || serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKBUYER) && serializedEscrow.vchLinkAlias != theEscrow.vchBuyerAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4061 - " + _("Only buyer can leave this feedback");
						return true;
					}
					else if((serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKSELLER || serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKSELLER) && serializedEscrow.vchLinkAlias != theEscrow.vchSellerAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4062 - " + _("Only seller can leave this feedback");
						return true;
					}
					else if((serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKARBITER || serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKARBITER) && serializedEscrow.vchLinkAlias != theEscrow.vchArbiterAlias)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4063 - " + _("Only arbiter can leave this feedback");
						return true;
					}
					serializedEscrow.feedback[0].nHeight = nHeight;
					serializedEscrow.feedback[0].txHash = tx.GetHash();
					serializedEscrow.feedback[1].nHeight = nHeight;
					serializedEscrow.feedback[1].txHash = tx.GetHash();
					int numBuyerRatings, numSellerRatings, numArbiterRatings, feedbackBuyerCount, feedbackSellerCount, feedbackArbiterCount;				
					FindFeedback(theEscrow.feedback, numBuyerRatings, numSellerRatings, numArbiterRatings, feedbackBuyerCount, feedbackSellerCount, feedbackArbiterCount);

					// has this user already rated?
					if(numBuyerRatings > 0)
					{
						if(serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKBUYER)
							serializedEscrow.feedback[0].nRating = 0;
						if(serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKBUYER)
							serializedEscrow.feedback[1].nRating = 0;
					}
					if(numSellerRatings > 0)
					{
						if(serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKSELLER)
							serializedEscrow.feedback[0].nRating = 0;
						if(serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKSELLER)
							serializedEscrow.feedback[1].nRating = 0;
					}
					if(numArbiterRatings > 0)
					{
						if(serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKARBITER)
							serializedEscrow.feedback[0].nRating = 0;
						if(serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKARBITER)
							serializedEscrow.feedback[1].nRating = 0;
					}

					if(feedbackBuyerCount >= 10 && (serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKBUYER || serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKBUYER))
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4064 - " + _("Cannot exceed 10 buyer feedbacks");
						return true;
					}
					else if(feedbackSellerCount >= 10 && (serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKSELLER || serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKSELLER))
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4065 - " + _("Cannot exceed 10 seller feedbacks");
						return true;
					}
					else if(feedbackArbiterCount >= 10 && (serializedEscrow.feedback[0].nFeedbackUserFrom == FEEDBACKARBITER || serializedEscrow.feedback[1].nFeedbackUserFrom == FEEDBACKARBITER))
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4066 - " + _("Cannot exceed 10 arbiter feedbacks");
						return true;
					}
					if(!dontaddtodb)
						HandleEscrowFeedback(serializedEscrow, theEscrow, vtxPos);	
					return true;
				}
			}
			else
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4067 - " + _("Escrow not found when trying to update");
				return true;
			}
					
		}
		else
		{

			if (pescrowdb->ExistsEscrow(vvchArgs[0]))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4068 - " + _("Escrow already exists");
				return true;
			}
			if(!theEscrow.txBTCId.IsNull())
			{
				if(pescrowdb->ExistsEscrowTx(theEscrow.txBTCId) || pofferdb->ExistsOfferTx(theEscrow.txBTCId))
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4069 - " + _("BTC Transaction ID specified was already used to pay for an offer");
					return true;
				}
				if(!dontaddtodb && !pescrowdb->WriteEscrowTx(theEscrow.vchEscrow, theEscrow.txBTCId))
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4070 - " + _("Failed to BTC Transaction ID to DB");		
					return error(errorMessage.c_str());
				}
			}

			if(theEscrow.nQty <= 0)
				theEscrow.nQty = 1;
			vector<COffer> myVtxPos;
			// make sure offer is still valid and then deduct qty
			if (GetTxAndVtxOfOffer( theEscrow.vchOffer, dbOffer, txOffer, myVtxPos))
			{
				dbOffer.nHeight = theEscrow.nAcceptHeight;
				dbOffer.GetOfferFromList(myVtxPos);
				if(dbOffer.sCategory.size() > 0 && boost::algorithm::starts_with(stringFromVch(dbOffer.sCategory), "wanted"))
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4071 - " + _("Cannot purchase a wanted offer");
					return true;
				}
				else if(dbOffer.nQty != -1)
				{
					vector<COffer> myLinkVtxPos;
					unsigned int nQty = dbOffer.nQty - theEscrow.nQty;
					// if this is a linked offer we must update the linked offer qty aswell
					if (pofferdb->ExistsOffer(dbOffer.vchLinkOffer)) {
						if (pofferdb->ReadOffer(dbOffer.vchLinkOffer, myLinkVtxPos) && !myLinkVtxPos.empty())
						{
							COffer myLinkOffer = myLinkVtxPos.back();
							myLinkOffer.nQty -= theEscrow.nQty;
							if(myLinkOffer.nQty < 0)
								myLinkOffer.nQty = 0;
							nQty = myLinkOffer.nQty;
							myLinkOffer.PutToOfferList(myLinkVtxPos);
							if (!dontaddtodb && !pofferdb->WriteOffer(dbOffer.vchLinkOffer, myLinkVtxPos))
							{
								errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4072 - " + _("Failed to write to offer link to DB");
								return true;
							}
							// go through the linked offers, if any, and update the linked offer qty based on the this qty
							for(unsigned int i=0;i<myLinkOffer.offerLinks.size();i++) {
								vector<COffer> myVtxPos;	
								if (pofferdb->ExistsOffer(myLinkOffer.offerLinks[i]) && myLinkOffer.offerLinks[i] != theEscrow.vchOffer) {
									if (pofferdb->ReadOffer(myLinkOffer.offerLinks[i], myVtxPos))
									{
										COffer offerLink = myVtxPos.back();					
										offerLink.nQty = nQty;	
										offerLink.PutToOfferList(myVtxPos);
										if (!dontaddtodb && !pofferdb->WriteOffer(myLinkOffer.offerLinks[i], myVtxPos))
										{
											errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4073 - " + _("Failed to write to offer link to DB");		
											return error(errorMessage.c_str());
										}
									}
								}
							}							
						}
					}
					// go through the linked offers, if any, and update the linked offer qty based on the this qty
					for(unsigned int i=0;i<dbOffer.offerLinks.size();i++) {
						vector<COffer> myVtxPos;	
						if (pofferdb->ExistsOffer(dbOffer.offerLinks[i])) {
							if (pofferdb->ReadOffer(dbOffer.offerLinks[i], myVtxPos))
							{
								COffer offerLink = myVtxPos.back();					
								offerLink.nQty = nQty;	
								offerLink.PutToOfferList(myVtxPos);
								if (!dontaddtodb && !pofferdb->WriteOffer(dbOffer.offerLinks[i], myVtxPos))
								{
									errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4074 - " + _("Failed to write to offer link to DB");		
									return error(errorMessage.c_str());
								}
							}
						}
					}
					dbOffer.nQty = nQty;
					if(dbOffer.nQty < 0)
						dbOffer.nQty = 0;
					dbOffer.PutToOfferList(myVtxPos);
					if (!dontaddtodb && !pofferdb->WriteOffer(theEscrow.vchOffer, myVtxPos))
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4075 - " + _("Failed to write to offer to DB");
						return true;
					}			
				}
			}
			else
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4076 - " + _("Cannot find offer for this escrow. It may be expired");
				return true;
			}
		}
	
        // set the escrow's txn-dependent values
		theEscrow.op = escrowOp;
		theEscrow.txHash = tx.GetHash();
		theEscrow.nHeight = nHeight;
		PutToEscrowList(vtxPos, theEscrow);
        // write escrow  
		
        if (!dontaddtodb && !pescrowdb->WriteEscrow(vvchArgs[0], vtxPos))
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4077 - " + _("Failed to write to escrow DB");
			return true;
		}
		if(fDebug)
			LogPrintf( "CONNECTED ESCROW: op=%s escrow=%s hash=%s height=%d\n",
                escrowFromOp(op).c_str(),
                stringFromVch(vvchArgs[0]).c_str(),
                tx.GetHash().ToString().c_str(),
                nHeight);
	}
    return true;
}
void HandleEscrowFeedback(const CEscrow& serializedEscrow, CEscrow& dbEscrow, vector<CEscrow> &vtxPos)
{
	for(int i =0;i<serializedEscrow.feedback.size();i++)
	{
		if(serializedEscrow.feedback[i].nRating > 0)
		{
			CSyscoinAddress address;
			if(serializedEscrow.feedback[i].nFeedbackUserTo == FEEDBACKBUYER)
				address = CSyscoinAddress(stringFromVch(dbEscrow.vchBuyerAlias));
			else if(serializedEscrow.feedback[i].nFeedbackUserTo == FEEDBACKSELLER)
				address = CSyscoinAddress(stringFromVch(dbEscrow.vchSellerAlias));
			else if(serializedEscrow.feedback[i].nFeedbackUserTo == FEEDBACKARBITER)
				address = CSyscoinAddress(stringFromVch(dbEscrow.vchArbiterAlias));
			if(address.IsValid() && address.isAlias)
			{
				vector<CAliasIndex> vtxPos;
				const vector<unsigned char> &vchAlias = vchFromString(address.aliasName);
				if (paliasdb->ReadAlias(vchAlias, vtxPos) && !vtxPos.empty())
				{
					
					CAliasIndex alias = vtxPos.back();
					if(serializedEscrow.feedback[i].nFeedbackUserTo == FEEDBACKBUYER)
					{
						alias.nRatingCountAsBuyer++;
						alias.nRatingAsBuyer += serializedEscrow.feedback[i].nRating;
					}
					else if(serializedEscrow.feedback[i].nFeedbackUserTo == FEEDBACKSELLER)
					{
						alias.nRatingCountAsSeller++;
						alias.nRatingAsSeller += serializedEscrow.feedback[i].nRating;
					}					
					else if(serializedEscrow.feedback[i].nFeedbackUserTo == FEEDBACKARBITER)
					{
						alias.nRatingCountAsArbiter++;
						alias.nRatingAsArbiter += serializedEscrow.feedback[i].nRating;
					}


					PutToAliasList(vtxPos, alias);
					paliasdb->WriteAlias(vchAlias, vchFromString(address.ToString()), vtxPos);
				}
			}
				
		}
		dbEscrow.feedback.push_back(serializedEscrow.feedback[i]);
	}
	PutToEscrowList(vtxPos, dbEscrow);
	pescrowdb->WriteEscrow(dbEscrow.vchEscrow, vtxPos);
}
UniValue generateescrowmultisig(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 4 )
        throw runtime_error(
		"generateescrowmultisig <buyer> <offer guid> <qty> <arbiter>\n"
                        + HelpRequiringPassphrase());

	vector<unsigned char> vchBuyer = vchFromValue(params[0]);
	vector<unsigned char> vchOffer = vchFromValue(params[1]);
	unsigned int nQty = 1;

	try {
		nQty = boost::lexical_cast<unsigned int>(params[2].get_str());
	} catch (std::exception &e) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4500 - " + _("Invalid quantity value. Quantity must be less than 4294967296."));
	}
	vector<unsigned char> vchArbiter = vchFromValue(params[3]);

	CAliasIndex arbiteralias;
	CTransaction arbiteraliastx;
	if (!GetTxOfAlias(vchArbiter, arbiteralias, arbiteraliastx))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4501 - " + _("Failed to read arbiter alias from DB"));

	CAliasIndex buyeralias;
	CTransaction buyeraliastx;
	if (!GetTxOfAlias(vchBuyer, buyeralias, buyeraliastx))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4502 - " + _("Failed to read arbiter alias from DB"));
	
	CTransaction txOffer, txAlias;
	vector<COffer> offerVtxPos;
	COffer theOffer, linkedOffer;
	if (!GetTxAndVtxOfOffer( vchOffer, theOffer, txOffer, offerVtxPos, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4503 - " + _("Could not find an offer with this identifier"));

	CAliasIndex selleralias;
	if (!GetTxOfAlias( theOffer.vchAlias, selleralias, txAlias, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4504 - " + _("Could not find seller alias with this identifier"));
	
	COfferLinkWhitelistEntry foundEntry;
	if(!theOffer.vchLinkOffer.empty())
	{
		CTransaction tmpTx;
		vector<COffer> offerTmpVtxPos;
		if (!GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkedOffer, tmpTx, offerTmpVtxPos, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4505 - " + _("Trying to accept a linked offer but could not find parent offer"));

		CAliasIndex theLinkedAlias;
		CTransaction txLinkedAlias;
		if (!GetTxOfAlias( linkedOffer.vchAlias, theLinkedAlias, txLinkedAlias, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4506 - " + _("Could not find an alias with this identifier"));
		

		selleralias = theLinkedAlias;
	}
	else
	{
		// if offer is not linked, look for a discount for the buyer
		theOffer.linkWhitelist.GetLinkEntryByHash(buyeralias.vchAlias, foundEntry);

	}
	CPubKey ArbiterPubKey(arbiteralias.vchPubKey);
	CPubKey SellerPubKey(selleralias.vchPubKey);
	CPubKey BuyerPubKey(buyeralias.vchPubKey);
	CScript scriptArbiter = GetScriptForDestination(ArbiterPubKey.GetID());
	CScript scriptSeller = GetScriptForDestination(SellerPubKey.GetID());
	CScript scriptBuyer = GetScriptForDestination(BuyerPubKey.GetID());
	UniValue arrayParams(UniValue::VARR);
	UniValue arrayOfKeys(UniValue::VARR);

	// standard 2 of 3 multisig
	arrayParams.push_back(2);
	arrayOfKeys.push_back(HexStr(arbiteralias.vchPubKey));
	arrayOfKeys.push_back(HexStr(selleralias.vchPubKey));
	arrayOfKeys.push_back(HexStr(buyeralias.vchPubKey));
	arrayParams.push_back(arrayOfKeys);
	UniValue resCreate;
	CScript redeemScript;
	try
	{
		resCreate = tableRPC.execute("createmultisig", arrayParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}
	if (!resCreate.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4507 - " + _("Could not create escrow transaction: Invalid response from createescrow"));

	int precision = 2;
	float fEscrowFee = getEscrowFee(theOffer.vchAliasPeg, vchFromString("BTC"), chainActive.Tip()->nHeight, precision);
	CAmount nTotal = theOffer.GetPrice(foundEntry)*nQty;
	CAmount nEscrowFee = GetEscrowArbiterFee(nTotal, fEscrowFee);
	CAmount nBTCFee = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), nEscrowFee, chainActive.Tip()->nHeight, precision);
	CAmount nBTCTotal = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), theOffer.GetPrice(foundEntry), chainActive.Tip()->nHeight, precision)*nQty;
	int nBTCFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("BTC"), chainActive.Tip()->nHeight, precision);
	// multisig spend is about 400 bytes
	nBTCTotal += nBTCFee + (nBTCFeePerByte*400);
	resCreate.push_back(Pair("total", ValueFromAmount(nBTCTotal)));
	resCreate.push_back(Pair("height", chainActive.Tip()->nHeight));
	return resCreate;
}

UniValue escrownew(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() < 5 ||  params.size() > 8)
        throw runtime_error(
		"escrownew <alias> <offer> <quantity> <message> <arbiter alias> [btcTx] [redeemScript] [height]\n"
						"<alias> An alias you own.\n"
                        "<offer> GUID of offer that this escrow is managing.\n"
                        "<quantity> Quantity of items to buy of offer.\n"
						"<message> Delivery details to seller.\n"
						"<arbiter alias> Alias of Arbiter.\n"
						"<btcTx> If paid in Bitcoin enter raw Bitcoin input transaction.\n"
						"<redeemScript> If paid in Bitcoin enter, enter redeemScript that generateescrowmultisig returns\n"
						"<height> If paid in Bitcoin enter, enter height that generateescrowmultisig returns\n"
                        + HelpRequiringPassphrase());
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	vector<unsigned char> vchOffer = vchFromValue(params[1]);
	uint64_t nHeight = chainActive.Tip()->nHeight;
	string strArbiter = params[4].get_str();
	boost::algorithm::to_lower(strArbiter);
	// check for alias existence in DB
	CAliasIndex arbiteralias;
	CTransaction aliastx, buyeraliastx;
	if (!GetTxOfAlias(vchFromString(strArbiter), arbiteralias, aliastx))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4508 - " + _("Failed to read arbiter alias from DB"));
	

	vector<unsigned char> vchMessage = vchFromValue(params[3]);
	vector<unsigned char> vchBTCTx;
	if(params.size() >= 6) 
		vchBTCTx = vchFromValue(params[5]);
	vector<unsigned char> vchRedeemScript;
	if(params.size() >= 7)
		vchRedeemScript = vchFromValue(params[6]);
	if(params.size() >= 8)
		nHeight = boost::lexical_cast<uint64_t>(params[7].get_str());
	CTransaction rawTx;
	if (!vchBTCTx.empty() && !DecodeHexTx(rawTx,stringFromVch(vchBTCTx)))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4509 - " + _("Could not find decode raw BTC transaction"));
	unsigned int nQty = 1;

	try {
		nQty = boost::lexical_cast<unsigned int>(params[2].get_str());
	} catch (std::exception &e) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4510 - " + _("Invalid quantity value. Quantity must be less than 4294967296."));
	}

    if (vchMessage.size() <= 0)
        vchMessage = vchFromString("ESCROW");


	CAliasIndex buyeralias;
	if (!GetTxOfAlias(vchAlias, buyeralias, buyeraliastx))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4511 - " + _("Could not find buyer alias with this name"));

	CPubKey buyerKey(buyeralias.vchPubKey);
    if(!IsSyscoinTxMine(buyeraliastx, "alias")) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4512 - " + _("This alias is not yours."));
    }
	if (pwalletMain->GetWalletTx(buyeraliastx.GetHash()) == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4513 - " + _("This alias is not in your wallet"));

	COffer theOffer, linkedOffer;
	
	CTransaction txOffer, txAlias;
	vector<COffer> offerVtxPos;
	if (!GetTxAndVtxOfOffer( vchOffer, theOffer, txOffer, offerVtxPos, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4514 - " + _("Could not find an offer with this identifier"));

	CAliasIndex selleralias;
	if (!GetTxOfAlias( theOffer.vchAlias, selleralias, txAlias, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4515 - " + _("Could not find seller alias with this identifier"));

	unsigned int memPoolQty = QtyOfPendingAcceptsInMempool(vchOffer);
	if(theOffer.nQty != -1 && theOffer.nQty < (nQty+memPoolQty))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4516 - " + _("Not enough remaining quantity to fulfill this escrow"));

	if(theOffer.sCategory.size() > 0 && boost::algorithm::starts_with(stringFromVch(theOffer.sCategory), "wanted"))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4517 - " + _("Cannot purchase a wanted offer"));

	const CWalletTx *wtxAliasIn = NULL;

	CScript scriptPubKeyAlias, scriptPubKeyAliasOrig;
	COfferLinkWhitelistEntry foundEntry;
	if(!theOffer.vchLinkOffer.empty())
	{
	
		CTransaction tmpTx;
		vector<COffer> offerTmpVtxPos;
		if (!GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkedOffer, tmpTx, offerTmpVtxPos, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4518 - " + _("Trying to accept a linked offer but could not find parent offer"));

		CAliasIndex theLinkedAlias;
		CTransaction txLinkedAlias;
		if (!GetTxOfAlias( linkedOffer.vchAlias, theLinkedAlias, txLinkedAlias, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4519 - " + _("Could not find an alias with this identifier"));
		if(linkedOffer.sCategory.size() > 0 && boost::algorithm::starts_with(stringFromVch(linkedOffer.sCategory), "wanted"))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4520 - " + _("Cannot purchase a wanted offer"));
	
		
		selleralias = theLinkedAlias;
	}
	else
	{
		// if offer is not linked, look for a discount for the buyer
		theOffer.linkWhitelist.GetLinkEntryByHash(buyeralias.vchAlias, foundEntry);

		if(!foundEntry.IsNull())
		{
			// make sure its in your wallet (you control this alias)
			if (IsSyscoinTxMine(buyeraliastx, "alias")) 
			{
				wtxAliasIn = pwalletMain->GetWalletTx(buyeraliastx.GetHash());		
				scriptPubKeyAliasOrig = GetScriptForDestination(buyerKey.GetID());
				scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << buyeralias.vchAlias  << buyeralias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
				scriptPubKeyAlias += scriptPubKeyAliasOrig;
			}			
		}
	}

	
    // gather inputs
	vector<unsigned char> vchEscrow = vchFromString(GenerateSyscoinGuid());

    // this is a syscoin transaction
    CWalletTx wtx;
	EnsureWalletIsUnlocked();
    CScript scriptPubKey, scriptPubKeyBuyer, scriptPubKeySeller, scriptPubKeyArbiter,scriptBuyer, scriptSeller,scriptArbiter;

	string strCipherText = "";
	// encrypt to offer owner
	if(!EncryptMessage(selleralias.vchPubKey, vchMessage, strCipherText))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4521 - " + _("Could not encrypt message to seller"));
	
	if (strCipherText.size() > MAX_ENCRYPTED_VALUE_LENGTH)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4522 - " + _("Payment message length cannot exceed 1023 bytes"));

	CPubKey ArbiterPubKey(arbiteralias.vchPubKey);
	CPubKey SellerPubKey(selleralias.vchPubKey);
	CPubKey BuyerPubKey(buyeralias.vchPubKey);

	scriptArbiter= GetScriptForDestination(ArbiterPubKey.GetID());
	scriptSeller= GetScriptForDestination(SellerPubKey.GetID());
	scriptBuyer= GetScriptForDestination(BuyerPubKey.GetID());
	vector<unsigned char> redeemScript;
	if(vchRedeemScript.empty())
	{
		UniValue arrayParams(UniValue::VARR);
		arrayParams.push_back(stringFromVch(buyeralias.vchAlias));
		arrayParams.push_back(stringFromVch(vchOffer));
		arrayParams.push_back( boost::lexical_cast<string>(nQty));
		arrayParams.push_back(stringFromVch(arbiteralias.vchAlias));
		UniValue resCreate;
		try
		{
			resCreate = tableRPC.execute("generateescrowmultisig", arrayParams);
		}
		catch (UniValue& objError)
		{
			throw runtime_error(find_value(objError, "message").get_str());
		}
		if (!resCreate.isObject())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4523 - " + _("Could not generate escrow multisig address: Invalid response from generateescrowmultisig"));
		const UniValue &o = resCreate.get_obj();
		const UniValue& redeemScript_value = find_value(o, "redeemScript");
		if (redeemScript_value.isStr())
		{
			redeemScript = ParseHex(redeemScript_value.get_str());
		}
		else
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4524 - " + _("Could not create escrow transaction: could not find redeem script in response"));
	}
	else
	{
			redeemScript = ParseHex(stringFromVch(vchRedeemScript));		
	}
	scriptPubKey = CScript(redeemScript.begin(), redeemScript.end());
	int precision = 2;
	// send to escrow address
	CAmount nTotal = theOffer.GetPrice(foundEntry)*nQty;
	float fEscrowFee = getEscrowFee(theOffer.vchAliasPeg, theOffer.sCurrencyCode, chainActive.Tip()->nHeight, precision);
	CAmount nEscrowFee = GetEscrowArbiterFee(nTotal, fEscrowFee);
	int nFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("SYS"), chainActive.Tip()->nHeight,precision);

	vector<CRecipient> vecSend;
	CRecipient recipientFee;
	CreateRecipient(scriptPubKey, recipientFee);
	CAmount nAmountWithFee = nTotal+nEscrowFee+(nFeePerByte*400);
	CWalletTx escrowWtx;
	CRecipient recipientEscrow  = {scriptPubKey, nAmountWithFee, false};
	if(vchBTCTx.empty())
		vecSend.push_back(recipientEscrow);
	
	// send to seller/arbiter so they can track the escrow through GUI
    // build escrow
    CEscrow newEscrow;
	newEscrow.op = OP_ESCROW_ACTIVATE;
	newEscrow.vchEscrow = vchEscrow;
	newEscrow.vchBuyerAlias = buyeralias.vchAlias;
	newEscrow.vchArbiterAlias = arbiteralias.vchAlias;
	newEscrow.vchRedeemScript = redeemScript;
	newEscrow.vchOffer = vchOffer;
	newEscrow.vchSellerAlias = selleralias.vchAlias;
	newEscrow.vchPaymentMessage = vchFromString(strCipherText);
	newEscrow.nQty = nQty;
	newEscrow.escrowInputTx = stringFromVch(vchBTCTx);
	newEscrow.nHeight = nHeight;
	newEscrow.nAcceptHeight = chainActive.Tip()->nHeight;
	if(!vchBTCTx.empty())
	{
		newEscrow.txBTCId = rawTx.GetHash();
	}
	const vector<unsigned char> &data = newEscrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));
	scriptPubKeyBuyer << CScript::EncodeOP_N(OP_ESCROW_ACTIVATE) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
	scriptPubKeySeller << CScript::EncodeOP_N(OP_ESCROW_ACTIVATE) << vchEscrow  << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
	scriptPubKeyArbiter << CScript::EncodeOP_N(OP_ESCROW_ACTIVATE) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
	scriptPubKeySeller += scriptSeller;
	scriptPubKeyArbiter += scriptArbiter;
	scriptPubKeyBuyer += scriptBuyer;


	// send the tranasction
	
	CRecipient recipientArbiter;
	CreateRecipient(scriptPubKeyArbiter, recipientArbiter);
	vecSend.push_back(recipientArbiter);
	CRecipient recipientSeller;
	CreateRecipient(scriptPubKeySeller, recipientSeller);
	vecSend.push_back(recipientSeller);
	CRecipient recipientBuyer;
	CreateRecipient(scriptPubKeyBuyer, recipientBuyer);
	vecSend.push_back(recipientBuyer);

	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	// if we use an alias as input to this escrow tx, we need another utxo for further alias transactions on this alias, so we create one here
	if(wtxAliasIn != NULL)
		vecSend.push_back(aliasRecipient);


	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInEscrow=NULL;
	SendMoneySyscoin(vecSend,recipientBuyer.nAmount+recipientArbiter.nAmount+recipientSeller.nAmount+aliasRecipient.nAmount+recipientEscrow.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxInEscrow);
	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	res.push_back(stringFromVch(vchEscrow));
	return res;
}
UniValue escrowrelease(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 1)
        throw runtime_error(
		"escrowrelease <escrow guid>\n"
                        "Releases escrow funds to seller, seller needs to sign the output transaction and send to the network.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);

    // this is a syscoin transaction
    CWalletTx wtx;

	EnsureWalletIsUnlocked();

    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
	vector<CEscrow> vtxPos;
    if (!GetTxAndVtxOfEscrow( vchEscrow, 
		escrow, tx, vtxPos))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4525 - " + _("Could not find a escrow with this key"));
	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4526 - " + _("This escrow is not in your wallet"));

    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, vtxPos.front().txHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4527 - " + _("Failed to find escrow transaction"));
	bool foundWhitelistAlias = false;
	for (unsigned int i = 0; i < fundingTx.vin.size(); i++) {
		vector<vector<unsigned char> > vvchIn;
		int opIn;
		const COutPoint *prevOutput = &fundingTx.vin[i].prevout;
		if(!GetPreviousInput(prevOutput, opIn, vvchIn))
			continue;
		if (IsAliasOp(opIn) && escrow.vchBuyerAlias == vvchIn[0])
		{
			foundWhitelistAlias = true; 
			break;
		}
	}

	CAliasIndex sellerAlias, buyerAlias, arbiterAlias, resellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction selleraliastx, buyeraliastx, arbiteraliastx, reselleraliastx;
	bool isExpired;
	CSyscoinAddress arbiterAddress, sellerAddress, buyerAddress, resellerAddress;
	CPubKey arbiterKey;
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
	}

	aliasVtxPos.clear();
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
	}
	aliasVtxPos.clear();
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAddress = CSyscoinAddress(sellerKey.GetID());
	}

	const CWalletTx *wtxAliasIn = NULL;
	CScript scriptPubKeyAlias;

	int nOutMultiSig = 0;
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	COfferLinkWhitelistEntry foundEntry;
	COffer theOffer, linkOffer;
	CTransaction txOffer;
	vector<COffer> offerVtxPos;
	if (!GetTxAndVtxOfOffer( escrow.vchOffer, theOffer, txOffer, offerVtxPos, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4528 - " + _("Could not find an offer with this identifier"));
	theOffer.nHeight = vtxPos.front().nAcceptHeight;
	theOffer.GetOfferFromList(offerVtxPos);
	CAmount nCommission;		
	if(theOffer.vchLinkOffer.empty())
	{
		// only apply whitelist discount if buyer had used his alias as input into the escrow
		if(foundWhitelistAlias)
			theOffer.linkWhitelist.GetLinkEntryByHash(buyerAlias.vchAlias, foundEntry);
		nCommission = 0;
	}
	else 
	{
		vector<COffer> offerLinkVtxPos;
		if (!GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkOffer, txOffer, offerLinkVtxPos, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4529 - " + _("Could not find an offer with this identifier"));
		linkOffer.nHeight = vtxPos.front().nAcceptHeight;
		linkOffer.GetOfferFromList(offerLinkVtxPos);

		if(GetTxAndVtxOfAlias(theOffer.vchAlias, resellerAlias, reselleraliastx, aliasVtxPos, isExpired, true))
		{		
			CPubKey resellerKey = CPubKey(resellerAlias.vchPubKey);
			resellerAddress = CSyscoinAddress(resellerKey.GetID());
		}

		linkOffer.linkWhitelist.GetLinkEntryByHash(theOffer.vchAlias, foundEntry);
		nCommission = theOffer.GetPrice() - linkOffer.GetPrice(foundEntry);
	}
	int precision = 2;
	CRecipient recipientFee;
	CreateRecipient(redeemScriptPubKey, recipientFee);
	
	CAmount nExpectedCommissionAmount = nCommission*escrow.nQty;
	CAmount nExpectedAmount = theOffer.GetPrice(foundEntry)*escrow.nQty; 
	float fEscrowFee = getEscrowFee(theOffer.vchAliasPeg, theOffer.sCurrencyCode, vtxPos.front().nAcceptHeight, precision);
	CAmount nEscrowFee = GetEscrowArbiterFee(nExpectedAmount, fEscrowFee);
	int nFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("SYS"),  vtxPos.front().nAcceptHeight,precision);
	CAmount nEscrowTotal =  nExpectedAmount + nEscrowFee + (nFeePerByte*400);
	// if we can't get it in this blockchain, try full raw tx decode (bitcoin input raw tx)
	if (!escrow.escrowInputTx.empty())
	{
		nExpectedCommissionAmount = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), nCommission, vtxPos.front().nAcceptHeight, precision)*escrow.nQty;
		nExpectedAmount = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), theOffer.GetPrice(foundEntry), vtxPos.front().nAcceptHeight, precision)*escrow.nQty; 
		nEscrowFee = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), nEscrowFee, vtxPos.front().nAcceptHeight, precision);
		int nBTCFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("BTC"),  vtxPos.front().nAcceptHeight, precision);
		nEscrowTotal =  nExpectedAmount + nEscrowFee + (nBTCFeePerByte*400);
		if (!DecodeHexTx(fundingTx, escrow.escrowInputTx))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4530 - " + _("Could not find the escrow funding transaction in the blockchain database."));
	}

	for(unsigned int i=0;i<fundingTx.vout.size();i++)
	{
		if(fundingTx.vout[i].nValue == nEscrowTotal)
		{
			nOutMultiSig = i;
			break;
		}
	} 
	CAmount nAmount = fundingTx.vout[nOutMultiSig].nValue;
	if(nAmount != nEscrowTotal)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4531 - " + _("Expected amount of escrow does not match what is held in escrow. Expected amount: ") +  boost::lexical_cast<string>(nEscrowTotal));

	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());

	string strPrivateKey ;
	bool arbiterSigning = false;
	vector<unsigned char> vchLinkAlias;
	// who is initiating release arbiter or buyer?
	try
	{

		// try arbiter
		CKeyID keyID;
		if (!arbiterAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4532 - " + _("Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4533 - " + _("Private key for arbiter address is not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
		wtxAliasIn = pwalletMain->GetWalletTx(arbiteraliastx.GetHash());
		if (wtxAliasIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4534 - This alias is not in your wallet");
		CScript scriptPubKeyOrig;
		scriptPubKeyOrig= GetScriptForDestination(arbiterKey.GetID());
			
		scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << arbiterAlias.vchAlias << arbiterAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
		scriptPubKeyAlias += scriptPubKeyOrig;
		vchLinkAlias = arbiterAlias.vchAlias;
		arbiterSigning = true;
	}
	catch(...)
	{
		arbiterSigning = false;
		// otherwise try buyer
		CKeyID keyID;
		if (!buyerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4535 - " + _("Buyer or Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4536 - " + _("Buyer or Arbiter private keys not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
		wtxAliasIn = pwalletMain->GetWalletTx(buyeraliastx.GetHash());
		if (wtxAliasIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4537 - This alias is not in your wallet");
		CScript scriptPubKeyOrig;
		scriptPubKeyOrig= GetScriptForDestination(buyerKey.GetID());
			
		scriptPubKeyAlias = CScript() << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << buyerAlias.vchAlias << buyerAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
		scriptPubKeyAlias += scriptPubKeyOrig;
		vchLinkAlias = buyerAlias.vchAlias;

	}


	// create a raw tx that sends escrow amount to seller and collateral to buyer
    // inputs buyer txHash
	UniValue arrayCreateParams(UniValue::VARR);
	UniValue createTxInputsArray(UniValue::VARR);
	UniValue createTxInputUniValue(UniValue::VOBJ);
	UniValue createAddressUniValue(UniValue::VOBJ);
	createTxInputUniValue.push_back(Pair("txid", fundingTx.GetHash().ToString()));
	createTxInputUniValue.push_back(Pair("vout", nOutMultiSig));
	createTxInputsArray.push_back(createTxInputUniValue);
	if(arbiterSigning)
	{
		// if linked offer send commission to affiliate
		if(!theOffer.vchLinkOffer.empty())
		{
			createAddressUniValue.push_back(Pair(resellerAddress.ToString(), ValueFromAmount(nExpectedCommissionAmount)));
			createAddressUniValue.push_back(Pair(sellerAddress.ToString(), ValueFromAmount(nExpectedAmount-nExpectedCommissionAmount)));
		}
		else
			createAddressUniValue.push_back(Pair(sellerAddress.ToString(), ValueFromAmount(nExpectedAmount)));
		createAddressUniValue.push_back(Pair(arbiterAddress.ToString(), ValueFromAmount(nEscrowFee)));
	}
	else
	{
		// if linked offer send commission to affiliate
		if(!theOffer.vchLinkOffer.empty())
		{
			createAddressUniValue.push_back(Pair(resellerAddress.ToString(), ValueFromAmount(nExpectedCommissionAmount)));
			createAddressUniValue.push_back(Pair(sellerAddress.ToString(), ValueFromAmount(nExpectedAmount-nExpectedCommissionAmount)));
		}
		else
			createAddressUniValue.push_back(Pair(sellerAddress.ToString(), ValueFromAmount(nExpectedAmount)));
		createAddressUniValue.push_back(Pair(buyerAddress.ToString(), ValueFromAmount(nEscrowFee)));
	}

	arrayCreateParams.push_back(createTxInputsArray);
	arrayCreateParams.push_back(createAddressUniValue);
	UniValue resCreate;
	try
	{
		resCreate = tableRPC.execute("createrawtransaction", arrayCreateParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}	
	if (!resCreate.isStr())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4538 - " + _("Could not create escrow transaction: Invalid response from createrawtransaction"));
	string createEscrowSpendingTx = resCreate.get_str();

	// Buyer/Arbiter signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);

	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", fundingTx.GetHash().ToString()));
	signUniValue.push_back(Pair("vout", nOutMultiSig));
	signUniValue.push_back(Pair("scriptPubKey", strEscrowScriptPubKey));
	signUniValue.push_back(Pair("redeemScript", HexStr(escrow.vchRedeemScript)));
	arraySignParams.push_back(createEscrowSpendingTx);
	arraySignInputs.push_back(signUniValue);
	arraySignParams.push_back(arraySignInputs);
	arrayPrivateKeys.push_back(strPrivateKey);
	arraySignParams.push_back(arrayPrivateKeys);
	UniValue res;
	try
	{
		res = tableRPC.execute("signrawtransaction", arraySignParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4539 - " + _("Could not sign escrow transaction: ") + find_value(objError, "message").get_str());
	}	
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4540 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
	const UniValue& o = res.get_obj();
	string hex_str = "";

	const UniValue& hex_value = find_value(o, "hex");
	if (hex_value.isStr())
		hex_str = hex_value.get_str();


	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_RELEASE;
	escrow.rawTx = ParseHex(hex_str);
	escrow.nHeight = chainActive.Tip()->nHeight;
	escrow.vchLinkAlias = vchLinkAlias;

	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));

    CScript scriptPubKeyOrigSeller, scriptPubKeySeller, scriptPubKeyOrigArbiter, scriptPubKeyArbiter;
	scriptPubKeySeller= GetScriptForDestination(sellerKey.GetID());
	scriptPubKeyArbiter= GetScriptForDestination(arbiterKey.GetID());

    scriptPubKeyOrigSeller << CScript::EncodeOP_N(OP_ESCROW_RELEASE) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyOrigSeller += scriptPubKeySeller;

	scriptPubKeyOrigArbiter << CScript::EncodeOP_N(OP_ESCROW_RELEASE) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyOrigArbiter += scriptPubKeyArbiter;

	vector<CRecipient> vecSend;
	CRecipient recipientSeller;
	CreateRecipient(scriptPubKeyOrigSeller, recipientSeller);
	vecSend.push_back(recipientSeller);

	CRecipient recipientArbiter;
	CreateRecipient(scriptPubKeyOrigArbiter, recipientArbiter);
	vecSend.push_back(recipientArbiter);

	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	SendMoneySyscoin(vecSend, recipientSeller.nAmount+recipientArbiter.nAmount+fee.nAmount+aliasRecipient.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxIn);
	UniValue ret(UniValue::VARR);
	ret.push_back(wtx.GetHash().GetHex());
	return ret;
}
UniValue escrowclaimrelease(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 1)
        throw runtime_error(
		"escrowclaimrelease <escrow guid>\n"
                        "Claim escrow funds released from buyer or arbiter using escrowrelease.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);


	EnsureWalletIsUnlocked();
	UniValue ret(UniValue::VARR);
    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
	vector<CEscrow> vtxPos;
    if (!GetTxAndVtxOfEscrow( vchEscrow, 
		escrow, tx, vtxPos))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4541 - " + _("Could not find a escrow with this key"));

	CAliasIndex sellerAlias, buyerAlias, arbiterAlias, resellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction selleraliastx, buyeraliastx, arbiteraliastx, reselleraliastx;
	bool isExpired;
	CSyscoinAddress sellerAddress, buyerAddress, arbiterAddress, resellerAddress;
	CPubKey sellerKey, buyerKey, arbiterKey, resellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAddress = CSyscoinAddress(sellerKey.GetID());
	}
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
	}
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
	}
    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight,vtxPos.front().txHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4542 - " + _("Failed to find escrow transaction"));
	bool foundWhitelistAlias = false;
	for (unsigned int i = 0; i < fundingTx.vin.size(); i++) {
		vector<vector<unsigned char> > vvchIn;
		int opIn;
		const COutPoint *prevOutput = &fundingTx.vin[i].prevout;
		if(!GetPreviousInput(prevOutput, opIn, vvchIn))
			continue;
		if (IsAliasOp(opIn) && escrow.vchBuyerAlias == vvchIn[0])
		{
			foundWhitelistAlias = true; 
			break;
		}
	}
 	int nOutMultiSig = 0;
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	COfferLinkWhitelistEntry foundEntry;
	COffer theOffer, linkOffer;
	CTransaction txOffer;
	vector<COffer> offerVtxPos;
	if (!GetTxAndVtxOfOffer( escrow.vchOffer, theOffer, txOffer, offerVtxPos, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4543 - " + _("Could not find an offer with this identifier"));
	theOffer.nHeight = vtxPos.front().nAcceptHeight;
	theOffer.GetOfferFromList(offerVtxPos);
	CAmount nCommission;		
	if(theOffer.vchLinkOffer.empty())
	{
		// only apply whitelist discount if buyer had used his alias as input into the escrow
		if(foundWhitelistAlias)
			theOffer.linkWhitelist.GetLinkEntryByHash(buyerAlias.vchAlias, foundEntry);
		nCommission = 0;
	}
	else 
	{
		vector<COffer> offerLinkVtxPos;
		if (!GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkOffer, txOffer, offerLinkVtxPos, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4544 - " + _("Could not find an offer with this identifier"));
		linkOffer.nHeight = vtxPos.front().nAcceptHeight;
		linkOffer.GetOfferFromList(offerLinkVtxPos);

		if(GetTxAndVtxOfAlias(theOffer.vchAlias, resellerAlias, reselleraliastx, aliasVtxPos, isExpired, true))
		{
			resellerKey = CPubKey(resellerAlias.vchPubKey);
			resellerAddress = CSyscoinAddress(resellerKey.GetID());
		}

		linkOffer.linkWhitelist.GetLinkEntryByHash(theOffer.vchAlias, foundEntry);
		nCommission = theOffer.GetPrice() - linkOffer.GetPrice(foundEntry);
	}
	int precision = 2;
	CRecipient recipientFee;
	CreateRecipient(redeemScriptPubKey, recipientFee);
	
	CAmount nExpectedCommissionAmount = nCommission*escrow.nQty;
	CAmount nExpectedAmount = theOffer.GetPrice(foundEntry)*escrow.nQty; 
	float fEscrowFee = getEscrowFee(theOffer.vchAliasPeg, theOffer.sCurrencyCode, vtxPos.front().nAcceptHeight, precision);
	CAmount nEscrowFee = GetEscrowArbiterFee(nExpectedAmount, fEscrowFee);
	int nFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("SYS"),  vtxPos.front().nAcceptHeight,precision);
	CAmount nEscrowTotal =  nExpectedAmount + nEscrowFee + (nFeePerByte*400);
	// if we can't get it in this blockchain, try full raw tx decode (bitcoin input raw tx)
	if (!escrow.escrowInputTx.empty())
	{
		nExpectedCommissionAmount = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), nCommission, vtxPos.front().nAcceptHeight, precision)*escrow.nQty;
		nExpectedAmount = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), theOffer.GetPrice(foundEntry), vtxPos.front().nAcceptHeight, precision)*escrow.nQty; 
		nEscrowFee = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), nEscrowFee, vtxPos.front().nAcceptHeight, precision);
		int nBTCFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("BTC"),  vtxPos.front().nAcceptHeight, precision);
		nEscrowTotal =  nExpectedAmount + nEscrowFee + (nBTCFeePerByte*400);
		if (!DecodeHexTx(fundingTx, escrow.escrowInputTx))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4545 - " + _("Could not find the escrow funding transaction in the blockchain database."));
	}	
	for(unsigned int i=0;i<fundingTx.vout.size();i++)
	{
		if(fundingTx.vout[i].nValue == nEscrowTotal)
		{
			nOutMultiSig = i;
			break;
		}
	} 
	CAmount nAmount = fundingTx.vout[nOutMultiSig].nValue;
	if(nAmount != nEscrowTotal)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4546 - " + _("Expected amount of escrow does not match what is held in escrow. Expected amount: ") +  boost::lexical_cast<string>(nEscrowTotal));

	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());

	bool foundSellerPayment = false;
	bool foundCommissionPayment = false;
	bool foundFeePayment = false;	
	UniValue arrayDecodeParams(UniValue::VARR);
	arrayDecodeParams.push_back(HexStr(escrow.rawTx));
	UniValue decodeRes;
	try
	{
		decodeRes = tableRPC.execute("decoderawtransaction", arrayDecodeParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}
	if (!decodeRes.isObject())
	{
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4547 - " + _("Could not decode escrow transaction: Invalid response from decoderawtransaction"));
	}
	const UniValue& decodeo = decodeRes.get_obj();
	const UniValue& vout_value = find_value(decodeo, "vout");
	if (!vout_value.isArray())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4548 - " + _("Could not decode escrow transaction: Cannot find VOUT from transaction"));	
	const UniValue &vouts = vout_value.get_array();
    for (unsigned int idx = 0; idx < vouts.size(); idx++) {
        const UniValue& vout = vouts[idx];					
		const UniValue &voutObj = vout.get_obj();					
		const UniValue &voutValue = find_value(voutObj, "value");
		if(!voutValue.isNum())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4549 - " + _("Could not decode escrow transaction: Invalid VOUT value"));
		int64_t iVout = AmountFromValue(voutValue);
		UniValue scriptPubKeyValue = find_value(voutObj, "scriptPubKey");
		if(!scriptPubKeyValue.isObject())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4550 - " + _("Could not decode escrow transaction: Invalid scriptPubKey value"));
		const UniValue &scriptPubKeyValueObj = scriptPubKeyValue.get_obj();	
		const UniValue &addressesValue = find_value(scriptPubKeyValueObj, "addresses");
		if(!addressesValue.isArray())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4551 - " + _("Could not decode escrow transaction: Invalid addresses"));

		const UniValue &addresses = addressesValue.get_array();
		for (unsigned int idx = 0; idx < addresses.size(); idx++) {
			const UniValue& address = addresses[idx];
			if(!address.isStr())
				throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4552 - " + _("Could not decode escrow transaction: Invalid address"));
			const string &strAddress = address.get_str();
			CSyscoinAddress payoutAddress(strAddress);
			// check arb fee is paid to arbiter or buyer
			if(!foundFeePayment)
			{
				if(stringFromVch(arbiterAlias.vchAlias) == payoutAddress.aliasName && iVout >= nEscrowFee)
					foundFeePayment = true;
			}
			if(!foundFeePayment)
			{
				if(stringFromVch(buyerAlias.vchAlias) == payoutAddress.aliasName && iVout >= nEscrowFee)
					foundFeePayment = true;
			}	
			if(!theOffer.vchLinkOffer.empty())
			{
				if(!foundCommissionPayment)
				{
					if(stringFromVch(resellerAlias.vchAlias) == payoutAddress.aliasName && iVout >= nExpectedCommissionAmount)
					{
						foundCommissionPayment = true;
					}
				}
				if(!foundSellerPayment)
				{
					if(stringFromVch(sellerAlias.vchAlias) == payoutAddress.aliasName && iVout >= (nExpectedAmount-nExpectedCommissionAmount))
					{
						foundSellerPayment = true;
					}
				}
			}
			else if(!foundSellerPayment)
			{
				if(stringFromVch(sellerAlias.vchAlias) == payoutAddress.aliasName && iVout >= nExpectedAmount)
				{
					foundSellerPayment = true;
				}
			}
			
		}
	}
	if(!foundSellerPayment)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4553 - " + _("Expected payment amount not found in escrow"));	
	if(!foundFeePayment)    
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4554 - " + _("Expected fee payment to arbiter or buyer not found in escrow"));	
	if(!theOffer.vchLinkOffer.empty() && !foundCommissionPayment)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4555 - " + _("Expected commission to affiliate not found in escrow"));	
	
	CKeyID keyID;
	if (!sellerAddress.GetKeyID(keyID))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4556 - " + _("Seller address does not refer to a key"));
	CKey vchSecret;
	if (!pwalletMain->GetKey(keyID, vchSecret))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4557 - " + _("Private key for seller address is not known"));

	const string &strPrivateKey = CSyscoinSecret(vchSecret).ToString();
    // Seller signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);
	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", fundingTx.GetHash().ToString()));
	signUniValue.push_back(Pair("vout", nOutMultiSig));
	signUniValue.push_back(Pair("scriptPubKey", strEscrowScriptPubKey));
	signUniValue.push_back(Pair("redeemScript", HexStr(escrow.vchRedeemScript)));
	arraySignParams.push_back(HexStr(escrow.rawTx));
	arraySignInputs.push_back(signUniValue);
	arraySignParams.push_back(arraySignInputs);
	arrayPrivateKeys.push_back(strPrivateKey);
	arraySignParams.push_back(arrayPrivateKeys);
	UniValue res;
	try
	{
		res = tableRPC.execute("signrawtransaction", arraySignParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4558 - " + _("Could not sign escrow transaction: ") + find_value(objError, "message").get_str());
	}	
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4559 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
	const UniValue& o = res.get_obj();
	string hex_str = "";

	const UniValue& hex_value = find_value(o, "hex");
	if (hex_value.isStr())
		hex_str = hex_value.get_str();

	const UniValue& complete_value = find_value(o, "complete");
	bool bComplete = false;
	if (complete_value.isBool())
		bComplete = complete_value.get_bool();

	if(!bComplete)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4560 - " + _("Could not sign escrow transaction. It is showing as incomplete, you may not allowed to complete this request at this time"));

	CTransaction rawTx;
	DecodeHexTx(rawTx,hex_str);
	ret.push_back(hex_str);
	ret.push_back(rawTx.GetHash().GetHex());
	return ret;
	
	
}
UniValue escrowcompleterelease(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 2)
        throw runtime_error(
		"escrowcompleterelease <escrow guid> <rawtx> \n"
                         "Completes an escrow release by creating the escrow complete release transaction on syscoin blockchain.\n"
						 "<rawtx> Raw syscoin escrow transaction. Enter the raw tx result from escrowclaimrelease.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);
	string rawTx = params[1].get_str();
	CTransaction myRawTx;
	DecodeHexTx(myRawTx,rawTx);
    // this is a syscoin transaction
    CWalletTx wtx;

	EnsureWalletIsUnlocked();

    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
    if (!GetTxOfEscrow( vchEscrow, 
		escrow, tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4561 - " + _("Could not find a escrow with this key"));

	bool btcPayment = false;
	if (!escrow.escrowInputTx.empty())
		btcPayment = true;

	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4562 - " + _("This escrow is not in your wallet"));
    // unserialize escrow from txn
    CEscrow theEscrow;
    if(!theEscrow.UnserializeFromTx(tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4563 - " + _("Cannot unserialize escrow from transaction"));
	vector<CEscrow> vtxPos;
	if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
		  throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4564 - " + _("Failed to read from escrow DB"));
    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, vtxPos.front().txHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4565 - " + _("Failed to find escrow transaction"));

	CAliasIndex sellerAlias, buyerAlias, arbiterAlias, resellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction selleraliastx, buyeraliastx, arbiteraliastx, reselleraliastx;
	bool isExpired;
	CSyscoinAddress arbiterAddress, sellerAddress, buyerAddress;
	CPubKey arbiterKey;
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
	}

	aliasVtxPos.clear();
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
	}
	aliasVtxPos.clear();
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAddress = CSyscoinAddress(sellerKey.GetID());
	}
	

	string strPrivateKey ;
	const CWalletTx *wtxAliasIn = NULL;
	vector<unsigned char> vchLinkAlias;
	CScript scriptPubKeyAlias;
	
	// otherwise try seller
	CKeyID keyID;
	if (!sellerAddress.GetKeyID(keyID))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4566 - " + _("Seller address does not refer to a key"));
	CKey vchSecret;
	if (!pwalletMain->GetKey(keyID, vchSecret))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4567 - " + _("Seller private keys not known"));
	strPrivateKey = CSyscoinSecret(vchSecret).ToString();
	wtxAliasIn = pwalletMain->GetWalletTx(selleraliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4568 - This alias is not in your wallet");
	CScript scriptPubKeyOrig;
	scriptPubKeyOrig= GetScriptForDestination(sellerKey.GetID());
	
	scriptPubKeyAlias = CScript() << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << sellerAlias.vchAlias << sellerAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;
	vchLinkAlias = sellerAlias.vchAlias;
	

	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_COMPLETE;
	escrow.nHeight = chainActive.Tip()->nHeight;
	escrow.vchLinkAlias = vchLinkAlias;
	escrow.redeemTxId = myRawTx.GetHash();

    CScript scriptPubKeyBuyer, scriptPubKeySeller, scriptPubKeyArbiter;

	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));
    scriptPubKeyBuyer << CScript::EncodeOP_N(OP_ESCROW_RELEASE) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyBuyer += GetScriptForDestination(buyerKey.GetID());
    scriptPubKeySeller << CScript::EncodeOP_N(OP_ESCROW_RELEASE) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeySeller += GetScriptForDestination(sellerKey.GetID());
    scriptPubKeyArbiter << CScript::EncodeOP_N(OP_ESCROW_RELEASE) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyArbiter += GetScriptForDestination(arbiterKey.GetID());
	vector<CRecipient> vecSend;
	CRecipient recipientBuyer, recipientSeller, recipientArbiter;
	CreateRecipient(scriptPubKeyBuyer, recipientBuyer);
	vecSend.push_back(recipientBuyer);
	CreateRecipient(scriptPubKeySeller, recipientSeller);
	vecSend.push_back(recipientSeller);
	CreateRecipient(scriptPubKeyArbiter, recipientArbiter);
	vecSend.push_back(recipientArbiter);

	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+recipientSeller.nAmount+recipientArbiter.nAmount+fee.nAmount+aliasRecipient.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxIn);
	UniValue ret(UniValue::VARR);
	ret.push_back(wtx.GetHash().GetHex());
	// broadcast the payment transaction to syscoin network if not bitcoin transaction
	if (!btcPayment)
	{
		UniValue arraySendParams(UniValue::VARR);
		arraySendParams.push_back(rawTx);
		UniValue returnRes;
		try
		{
			returnRes = tableRPC.execute("sendrawtransaction", arraySendParams);
		}
		catch (UniValue& objError)
		{
			throw runtime_error(find_value(objError, "message").get_str());
		}
		if (!returnRes.isStr())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4569 - " + _("Could not send escrow transaction: Invalid response from sendrawtransaction"));
	}
	return ret;
}
UniValue escrowrefund(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 1)
        throw runtime_error(
		"escrowrefund <escrow guid>\n"
                         "Refunds escrow funds back to buyer, buyer needs to sign the output transaction and send to the network.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);
    // this is a syscoin transaction
    CWalletTx wtx;

	EnsureWalletIsUnlocked();

     // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
	vector<CEscrow> vtxPos;
    if (!GetTxAndVtxOfEscrow( vchEscrow, 
		escrow, tx, vtxPos))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4570 - " + _("Could not find a escrow with this key"));
	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4571 - " + _("This escrow is not in your wallet"));

    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, vtxPos.front().txHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4572 - " + _("Failed to find escrow transaction"));
	bool foundWhitelistAlias = false;
	for (unsigned int i = 0; i < fundingTx.vin.size(); i++) {
		vector<vector<unsigned char> > vvchIn;
		int opIn;
		const COutPoint *prevOutput = &fundingTx.vin[i].prevout;
		if(!GetPreviousInput(prevOutput, opIn, vvchIn))
			continue;
		if (IsAliasOp(opIn) && escrow.vchBuyerAlias == vvchIn[0])
		{
			foundWhitelistAlias = true; 
			break;
		}
	}	
	CAliasIndex sellerAlias, buyerAlias, arbiterAlias, resellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction selleraliastx, buyeraliastx, arbiteraliastx, reselleraliastx;
	bool isExpired;
	CSyscoinAddress arbiterAddress, sellerAddress, buyerAddress;
	CPubKey arbiterKey;
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
	}

	aliasVtxPos.clear();
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
	}
	aliasVtxPos.clear();
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAddress = CSyscoinAddress(sellerKey.GetID());
	}

	int nOutMultiSig = 0;
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	COfferLinkWhitelistEntry foundEntry;
	COffer theOffer, linkOffer;
	CTransaction txOffer;
	vector<COffer> offerVtxPos;
	if (!GetTxAndVtxOfOffer( escrow.vchOffer, theOffer, txOffer, offerVtxPos, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4573 - " + _("Could not find an offer with this identifier"));
	theOffer.nHeight = vtxPos.front().nAcceptHeight;
	theOffer.GetOfferFromList(offerVtxPos);	
	if(theOffer.vchLinkOffer.empty())
	{
		// only apply whitelist discount if buyer had used his alias as input into the escrow
		if(foundWhitelistAlias)
			theOffer.linkWhitelist.GetLinkEntryByHash(buyerAlias.vchAlias, foundEntry);
	}
	else 
	{
		vector<COffer> offerLinkVtxPos;
		if (!GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkOffer, txOffer, offerLinkVtxPos, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4574 - " + _("Could not find an offer with this identifier"));
		linkOffer.nHeight = vtxPos.front().nAcceptHeight;
		linkOffer.GetOfferFromList(offerLinkVtxPos);

		linkOffer.linkWhitelist.GetLinkEntryByHash(theOffer.vchAlias, foundEntry);
	}	
	int precision = 2;
	CRecipient recipientFee;
	CreateRecipient(redeemScriptPubKey, recipientFee);
	float fEscrowFee = getEscrowFee(theOffer.vchAliasPeg, theOffer.sCurrencyCode, vtxPos.front().nAcceptHeight, precision);
	CAmount nExpectedAmount = theOffer.GetPrice(foundEntry)*escrow.nQty; 
	CAmount nEscrowFee = GetEscrowArbiterFee(nExpectedAmount, fEscrowFee);
	int nFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("SYS"),  vtxPos.front().nAcceptHeight,precision);
	CAmount nEscrowTotal =  nExpectedAmount + nEscrowFee + (nFeePerByte*400);
	// if we can't get it in this blockchain, try full raw tx decode (bitcoin input raw tx)
	if (!escrow.escrowInputTx.empty())
	{
		nExpectedAmount = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), theOffer.GetPrice(foundEntry), vtxPos.front().nAcceptHeight, precision)*escrow.nQty; 
		nEscrowFee = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), nEscrowFee, vtxPos.front().nAcceptHeight, precision);
		int nBTCFeePerByte = getFeePerByte(theOffer.vchAliasPeg, vchFromString("BTC"),  vtxPos.front().nAcceptHeight, precision);
		nEscrowTotal =  nExpectedAmount + nEscrowFee + (nBTCFeePerByte*400);
		if (!DecodeHexTx(fundingTx, escrow.escrowInputTx))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4575 - " + _("Could not find the escrow funding transaction in the blockchain database."));
	}
	for(unsigned int i=0;i<fundingTx.vout.size();i++)
	{
		if(fundingTx.vout[i].nValue == nEscrowTotal)
		{
			nOutMultiSig = i;
			break;
		}
	} 
	CAmount nAmount = fundingTx.vout[nOutMultiSig].nValue;
	if(nAmount != nEscrowTotal)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4576 - " + _("Expected amount of escrow does not match what is held in escrow. Expected amount: ") +  boost::lexical_cast<string>(nEscrowTotal));

	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());
	string strPrivateKey ;
	bool arbiterSigning = false;
	const CWalletTx *wtxAliasIn = NULL;
	vector<unsigned char> vchLinkAlias;
	CScript scriptPubKeyAlias;
	// who is initiating release arbiter or seller?
	try
	{
		
		// try arbiter
		CKeyID keyID;
		if (!arbiterAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4577 - " + _("Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4578 - " + _("Private key for arbiter address is not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
		wtxAliasIn = pwalletMain->GetWalletTx(arbiteraliastx.GetHash());
		if (wtxAliasIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4579 - This alias is not in your wallet");
		CScript scriptPubKeyOrig;
		scriptPubKeyOrig= GetScriptForDestination(arbiterKey.GetID());
		
		scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << arbiterAlias.vchAlias << arbiterAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
		scriptPubKeyAlias += scriptPubKeyOrig;
		vchLinkAlias = arbiterAlias.vchAlias;
		arbiterSigning = true;
	}
	catch(...)
	{
		arbiterSigning = false;
		// otherwise try seller
		CKeyID keyID;
		if (!sellerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4580 - " + _("Seller or Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4581 - " + _("Seller or Arbiter private keys not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
		wtxAliasIn = pwalletMain->GetWalletTx(selleraliastx.GetHash());
		if (wtxAliasIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4582 - This alias is not in your wallet");
		CScript scriptPubKeyOrig;
		scriptPubKeyOrig= GetScriptForDestination(sellerKey.GetID());
		
		scriptPubKeyAlias = CScript() << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << sellerAlias.vchAlias << sellerAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
		scriptPubKeyAlias += scriptPubKeyOrig;
		vchLinkAlias = sellerAlias.vchAlias;
	}
	// refunds buyer from escrow
	UniValue arrayCreateParams(UniValue::VARR);
	UniValue createTxInputsArray(UniValue::VARR);
	UniValue createTxInputUniValue(UniValue::VOBJ);
	UniValue createAddressUniValue(UniValue::VOBJ);
	createTxInputUniValue.push_back(Pair("txid", fundingTx.GetHash().ToString()));
	createTxInputUniValue.push_back(Pair("vout", nOutMultiSig));
	createTxInputsArray.push_back(createTxInputUniValue);
	if(arbiterSigning)
	{
		createAddressUniValue.push_back(Pair(buyerAddress.ToString(), ValueFromAmount(nExpectedAmount)));
		createAddressUniValue.push_back(Pair(arbiterAddress.ToString(), ValueFromAmount(nEscrowFee)));
	}
	else
	{
		createAddressUniValue.push_back(Pair(buyerAddress.ToString(), ValueFromAmount(nExpectedAmount+nEscrowFee)));
	}	
	arrayCreateParams.push_back(createTxInputsArray);
	arrayCreateParams.push_back(createAddressUniValue);
	UniValue resCreate;
	try
	{
		resCreate = tableRPC.execute("createrawtransaction", arrayCreateParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}
	if (!resCreate.isStr())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4583 - " + _("Could not create escrow transaction: Invalid response from createrawtransaction"));
	string createEscrowSpendingTx = resCreate.get_str();

	// Buyer/Arbiter signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);

	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", fundingTx.GetHash().ToString()));
	signUniValue.push_back(Pair("vout", nOutMultiSig));
	signUniValue.push_back(Pair("scriptPubKey", strEscrowScriptPubKey));
	signUniValue.push_back(Pair("redeemScript", HexStr(escrow.vchRedeemScript)));
	arraySignParams.push_back(createEscrowSpendingTx);
	arraySignInputs.push_back(signUniValue);
	arraySignParams.push_back(arraySignInputs);
	arrayPrivateKeys.push_back(strPrivateKey);
	arraySignParams.push_back(arrayPrivateKeys);
	UniValue res;
	try
	{
		res = tableRPC.execute("signrawtransaction", arraySignParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4584 - " + _("Could not sign escrow transaction: ") + find_value(objError, "message").get_str());;
	}
	
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4585 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
	const UniValue& o = res.get_obj();
	string hex_str = "";

	const UniValue& hex_value = find_value(o, "hex");
	if (hex_value.isStr())
		hex_str = hex_value.get_str();

	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_REFUND;
	escrow.rawTx = ParseHex(hex_str);
	escrow.nHeight = chainActive.Tip()->nHeight;
	escrow.vchLinkAlias = vchLinkAlias;

	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));

    CScript scriptPubKeyOrigBuyer, scriptPubKeyBuyer, scriptPubKeyOrigArbiter, scriptPubKeyArbiter;
	scriptPubKeyBuyer= GetScriptForDestination(buyerKey.GetID());
	scriptPubKeyArbiter= GetScriptForDestination(arbiterKey.GetID());

    scriptPubKeyOrigBuyer << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyOrigBuyer += scriptPubKeyBuyer;

	scriptPubKeyOrigArbiter << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyOrigArbiter += scriptPubKeyArbiter;

	vector<CRecipient> vecSend;
	CRecipient recipientBuyer;
	CreateRecipient(scriptPubKeyOrigBuyer, recipientBuyer);
	vecSend.push_back(recipientBuyer);

	CRecipient recipientArbiter;
	CreateRecipient(scriptPubKeyOrigArbiter, recipientArbiter);
	vecSend.push_back(recipientArbiter);

	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+recipientArbiter.nAmount+fee.nAmount+aliasRecipient.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxIn);
	UniValue ret(UniValue::VARR);
	ret.push_back(wtx.GetHash().GetHex());
	return ret;
}
UniValue escrowclaimrefund(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 1)
        throw runtime_error(
		"escrowclaimrefund <escrow guid>\n"
                        "Claim escrow funds released from seller or arbiter using escrowrefund.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);


	EnsureWalletIsUnlocked();
	UniValue ret(UniValue::VARR);
    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
	vector<CEscrow> vtxPos;
    if (!GetTxAndVtxOfEscrow( vchEscrow, 
		escrow, tx, vtxPos))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4586 - " + _("Could not find a escrow with this key"));

	CAliasIndex sellerAlias, buyerAlias, arbiterAlias, resellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction selleraliastx, buyeraliastx, arbiteraliastx, reselleraliastx;
	bool isExpired;
	CSyscoinAddress arbiterAddress, sellerAddress, buyerAddress;
	CPubKey arbiterKey;
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
	}

	aliasVtxPos.clear();
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
	}
	aliasVtxPos.clear();
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAddress = CSyscoinAddress(sellerKey.GetID());
	}
	const CWalletTx* wtxAliasIn = pwalletMain->GetWalletTx(buyeraliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4587 - This alias is not in your wallet");

	CWalletTx wtx;
	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4588 - " + _("This escrow is not in your wallet"));

    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, vtxPos.front().txHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4589 - " + _("Failed to find escrow transaction"));
	bool foundWhitelistAlias = false;
	for (unsigned int i = 0; i < fundingTx.vin.size(); i++) {
		vector<vector<unsigned char> > vvchIn;
		int opIn;
		const COutPoint *prevOutput = &fundingTx.vin[i].prevout;
		if(!GetPreviousInput(prevOutput, opIn, vvchIn))
			continue;
		if (IsAliasOp(opIn) && escrow.vchBuyerAlias == vvchIn[0])
		{
			foundWhitelistAlias = true; 
			break;
		}
	}
 	int nOutMultiSig = 0;
	// 0.5% escrow fee
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	COfferLinkWhitelistEntry foundEntry;
	COffer theOffer, linkOffer;
	CTransaction txOffer;
	vector<COffer> offerVtxPos;
	if (!GetTxAndVtxOfOffer( escrow.vchOffer, theOffer, txOffer, offerVtxPos, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4590 - " + _("Could not find an offer with this identifier"));
	theOffer.nHeight = vtxPos.front().nAcceptHeight;
	theOffer.GetOfferFromList(offerVtxPos);		
	if(theOffer.vchLinkOffer.empty())
	{
		// only apply whitelist discount if buyer had used his alias as input into the escrow
		if(foundWhitelistAlias)
			theOffer.linkWhitelist.GetLinkEntryByHash(buyerAlias.vchAlias, foundEntry);
	}
	else 
	{
		vector<COffer> offerLinkVtxPos;
		if (!GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkOffer, txOffer, offerLinkVtxPos, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4591 - " + _("Could not find an offer with this identifier"));
		linkOffer.nHeight = vtxPos.front().nAcceptHeight;
		linkOffer.GetOfferFromList(offerLinkVtxPos);

		linkOffer.linkWhitelist.GetLinkEntryByHash(theOffer.vchAlias, foundEntry);
	}	

	CAmount nExpectedAmount = theOffer.GetPrice(foundEntry)*escrow.nQty; 
	if (!escrow.escrowInputTx.empty())
	{
		int precision = 2;
		nExpectedAmount = convertSyscoinToCurrencyCode(theOffer.vchAliasPeg, vchFromString("BTC"), theOffer.GetPrice(foundEntry), vtxPos.front().nAcceptHeight, precision)*escrow.nQty; 
		if (!DecodeHexTx(fundingTx, escrow.escrowInputTx))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4592 - " + _("Could not find the escrow funding transaction in the blockchain database."));
	}	

	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());
	// decode rawTx and check it pays enough and it pays to buyer appropriately
	// check that right amount is going to be sent to buyer
	UniValue arrayDecodeParams(UniValue::VARR);

	arrayDecodeParams.push_back(HexStr(escrow.rawTx));
	UniValue decodeRes;
	try
	{
		decodeRes = tableRPC.execute("decoderawtransaction", arrayDecodeParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}
	if (!decodeRes.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4593 - " + _("Could not decode escrow transaction: Invalid response from decoderawtransaction"));
	bool foundRefundPayment = false;
	const UniValue& decodeo = decodeRes.get_obj();
	const UniValue& vout_value = find_value(decodeo, "vout");
	if (!vout_value.isArray())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4594 - " + _("Could not decode escrow transaction: Cannot find VOUT from transaction"));	
	const UniValue &vouts = vout_value.get_array();
    for (unsigned int idx = 0; idx < vouts.size(); idx++) {
        const UniValue& vout = vouts[idx];					
		const UniValue &voutObj = vout.get_obj();					
		const UniValue &voutValue = find_value(voutObj, "value");
		if(!voutValue.isNum())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4595 - " + _("Could not decode escrow transaction: Invalid VOUT value"));
		int64_t iVout = AmountFromValue(voutValue);
		UniValue scriptPubKeyValue = find_value(voutObj, "scriptPubKey");
		if(!scriptPubKeyValue.isObject())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4596 - " + _("Could not decode escrow transaction: Invalid scriptPubKey value"));
		const UniValue &scriptPubKeyValueObj = scriptPubKeyValue.get_obj();	
		const UniValue &addressesValue = find_value(scriptPubKeyValueObj, "addresses");
		if(!addressesValue.isArray())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4597 - " + _("Could not decode escrow transaction: Invalid addresses"));

		const UniValue &addresses = addressesValue.get_array();
		for (unsigned int idx = 0; idx < addresses.size(); idx++) {
			const UniValue& address = addresses[idx];
			if(!address.isStr())
				throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4598 - " + _("Could not decode escrow transaction: Invalid address"));
			const string &strAddress = address.get_str();
			CSyscoinAddress payoutAddress(strAddress);
			if(!foundRefundPayment)
			{
				if(stringFromVch(buyerAlias.vchAlias) == payoutAddress.aliasName && iVout >= nExpectedAmount)
					foundRefundPayment = true;
			}	
		}
	}
	if(!foundRefundPayment)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4599 - " + _("Expected refund amount not found"));	


	// get buyer's private key for signing
	CKeyID keyID;
	if (!buyerAddress.GetKeyID(keyID))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4600 - " + _("Buyer address does not refer to a key"));
	CKey vchSecret;
	if (!pwalletMain->GetKey(keyID, vchSecret))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4601 - " + _("Private key for buyer address is not known"));
	string strPrivateKey = CSyscoinSecret(vchSecret).ToString();
    // buyer signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);
	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", fundingTx.GetHash().ToString()));
	signUniValue.push_back(Pair("vout", nOutMultiSig));
	signUniValue.push_back(Pair("scriptPubKey", strEscrowScriptPubKey));
	signUniValue.push_back(Pair("redeemScript", HexStr(escrow.vchRedeemScript)));
	arraySignParams.push_back(HexStr(escrow.rawTx));
	arraySignInputs.push_back(signUniValue);
	arraySignParams.push_back(arraySignInputs);
	arrayPrivateKeys.push_back(strPrivateKey);
	arraySignParams.push_back(arrayPrivateKeys);
	UniValue res;
	try
	{
		res = tableRPC.execute("signrawtransaction", arraySignParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4602 - " + _("Could not sign escrow transaction: ") + find_value(objError, "message").get_str());
	}
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4603 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
	const UniValue& o = res.get_obj();
	string hex_str = "";

	const UniValue& hex_value = find_value(o, "hex");
	if (hex_value.isStr())
		hex_str = hex_value.get_str();
	const UniValue& complete_value = find_value(o, "complete");
	bool bComplete = false;
	if (complete_value.isBool())
		bComplete = complete_value.get_bool();

	if(!bComplete)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4604 - " + _("Could not sign escrow transaction. It is showing as incomplete, you may not allowed to complete this request at this time"));

	CTransaction rawTx;
	DecodeHexTx(rawTx,hex_str);
	ret.push_back(hex_str);
	ret.push_back(rawTx.GetHash().GetHex());
	return ret;
}
UniValue escrowcompleterefund(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 2)
        throw runtime_error(
		"escrowcompleterefund <escrow guid> <rawtx> \n"
                         "Completes an escrow refund by creating the escrow complete refund transaction on syscoin blockchain.\n"
						 "<rawtx> Raw syscoin escrow transaction. Enter the raw tx result from escrowclaimrefund.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);
	string rawTx = params[1].get_str();
	CTransaction myRawTx;
	DecodeHexTx(myRawTx,rawTx);
    // this is a syscoin transaction
    CWalletTx wtx;

	EnsureWalletIsUnlocked();

    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
    if (!GetTxOfEscrow( vchEscrow, 
		escrow, tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4605 - " + _("Could not find a escrow with this key"));

	bool btcPayment = false;
	if (!escrow.escrowInputTx.empty())
		btcPayment = true;

	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4606 - " + _("This escrow is not in your wallet"));
    // unserialize escrow from txn
    CEscrow theEscrow;
    if(!theEscrow.UnserializeFromTx(tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4607 - " + _("Cannot unserialize escrow from transaction"));
	vector<CEscrow> vtxPos;
	if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
		  throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4608 - " + _("Failed to read from escrow DB"));
    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, vtxPos.front().txHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4609 - " + _("Failed to find escrow transaction"));

	CAliasIndex sellerAlias, buyerAlias, arbiterAlias, resellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction selleraliastx, buyeraliastx, arbiteraliastx, reselleraliastx;
	bool isExpired;
	CSyscoinAddress arbiterAddress, sellerAddress, buyerAddress;
	CPubKey arbiterKey;
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
	}

	aliasVtxPos.clear();
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
	}
	aliasVtxPos.clear();
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAddress = CSyscoinAddress(sellerKey.GetID());
	}
	

	string strPrivateKey ;
	const CWalletTx *wtxAliasIn = NULL;
	vector<unsigned char> vchLinkAlias;
	CScript scriptPubKeyAlias;
	
	CKeyID keyID;
	if (!buyerAddress.GetKeyID(keyID))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4610 - " + _("Buyer address does not refer to a key"));
	CKey vchSecret;
	if (!pwalletMain->GetKey(keyID, vchSecret))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4611 - " + _("Buyer private keys not known"));
	strPrivateKey = CSyscoinSecret(vchSecret).ToString();
	wtxAliasIn = pwalletMain->GetWalletTx(buyeraliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4612 - This alias is not in your wallet");
	CScript scriptPubKeyOrig;
	scriptPubKeyOrig= GetScriptForDestination(buyerKey.GetID());
	
	scriptPubKeyAlias = CScript() << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << buyerAlias.vchAlias << buyerAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;
	vchLinkAlias = buyerAlias.vchAlias;
	

	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_COMPLETE;
	escrow.nHeight = chainActive.Tip()->nHeight;
	escrow.vchLinkAlias = vchLinkAlias;
	escrow.redeemTxId = myRawTx.GetHash();

    CScript scriptPubKeyBuyer, scriptPubKeySeller, scriptPubKeyArbiter;

	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));
    scriptPubKeyBuyer << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyBuyer += GetScriptForDestination(buyerKey.GetID());
    scriptPubKeySeller << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeySeller += GetScriptForDestination(sellerKey.GetID());
    scriptPubKeyArbiter << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyArbiter += GetScriptForDestination(arbiterKey.GetID());
	vector<CRecipient> vecSend;
	CRecipient recipientBuyer, recipientSeller, recipientArbiter;
	CreateRecipient(scriptPubKeyBuyer, recipientBuyer);
	vecSend.push_back(recipientBuyer);
	CreateRecipient(scriptPubKeySeller, recipientSeller);
	vecSend.push_back(recipientSeller);
	CreateRecipient(scriptPubKeyArbiter, recipientArbiter);
	vecSend.push_back(recipientArbiter);

	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+recipientSeller.nAmount+recipientArbiter.nAmount+fee.nAmount+aliasRecipient.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxIn);
	UniValue ret(UniValue::VARR);
	ret.push_back(wtx.GetHash().GetHex());

	// broadcast the payment transaction to syscoin network if not bitcoin transaction
	if (!btcPayment)
	{
		UniValue arraySendParams(UniValue::VARR);
		arraySendParams.push_back(rawTx);
		UniValue returnRes;
		try
		{
			returnRes = tableRPC.execute("sendrawtransaction", arraySendParams);
		}
		catch (UniValue& objError)
		{
			throw runtime_error(find_value(objError, "message").get_str());
		}
		if (!returnRes.isStr())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4613 - " + _("Could not send escrow transaction: Invalid response from sendrawtransaction"));
	}

	return ret;
}
UniValue escrowfeedback(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 5)
        throw runtime_error(
		"escrowfeedback <escrow guid> [feedbackprimary] [ratingprimary] [feedbacksecondary] [ratingasecondary]\n"
                        "Send feedback for primary and secondary users in escrow, depending on who you are. Ratings are numbers from 1 to 5\n"
						"If you are the buyer, feedbackprimary is for seller and feedbacksecondary is for arbiter.\n"
						"If you are the seller, feedbackprimary is for buyer and feedbacksecondary is for arbiter.\n"
						"If you are the arbiter, feedbackprimary is for buyer and feedbacksecondary is for seller.\n"
						"If arbiter didn't do any work for this escrow you can leave his feedback empty and rating as a 0.\n"
                        + HelpRequiringPassphrase());
   // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);
	int nRatingPrimary = 0;
	int nRatingSecondary = 0;
	vector<unsigned char> vchFeedbackPrimary;
	vector<unsigned char> vchFeedbackSecondary;
	if(params.size() > 1)
		vchFeedbackPrimary = vchFromValue(params[1]);
	if(params.size() > 2)
	{
		try {
			nRatingPrimary = boost::lexical_cast<int>(params[2].get_str());

		} catch (std::exception &e) {
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4614 - " + _("Invalid primary rating value"));
		}
	}
	if(params.size() > 3)
		vchFeedbackSecondary = vchFromValue(params[3]);
	if(params.size() > 4)
	{
		try {
			nRatingSecondary = boost::lexical_cast<int>(params[4].get_str());

		} catch (std::exception &e) {
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4615 - " + _("Invalid secondary rating value"));
		}
	}
    // this is a syscoin transaction
    CWalletTx wtx;

	EnsureWalletIsUnlocked();

    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
    if (!GetTxOfEscrow( vchEscrow, 
		escrow, tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4616 - " + _("Could not find a escrow with this key"));

	CAliasIndex arbiterAliasLatest, buyerAliasLatest, sellerAliasLatest;
	CTransaction arbiteraliastx, selleraliastx, buyeraliastx;
	GetTxOfAlias(escrow.vchArbiterAlias, arbiterAliasLatest, arbiteraliastx, true);
	CPubKey arbiterKey(arbiterAliasLatest.vchPubKey);
	CSyscoinAddress arbiterAddress(arbiterKey.GetID());

	GetTxOfAlias(escrow.vchBuyerAlias, buyerAliasLatest, buyeraliastx, true);
	CPubKey buyerKey(buyerAliasLatest.vchPubKey);
	CSyscoinAddress buyerAddress(buyerKey.GetID());

	GetTxOfAlias(escrow.vchSellerAlias, sellerAliasLatest, selleraliastx, true);
	CPubKey sellerKey(sellerAliasLatest.vchPubKey);
	CSyscoinAddress sellerAddress(sellerKey.GetID());
	bool foundBuyerKey = false;
	vector <unsigned char> vchLinkAlias;
	CScript scriptPubKeyAlias;
	const CWalletTx *wtxAliasIn = NULL;
	try
	{
		CKeyID keyID;
		if (!buyerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4617 - " + _("Buyer address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4618 - " + _("Private key for buyer address is not known"));
		wtxAliasIn = pwalletMain->GetWalletTx(buyeraliastx.GetHash());
		if (wtxAliasIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4619 - " + _("Buyer alias is not in your wallet"));

		CScript scriptPubKeyAliasOrig= GetScriptForDestination(buyerKey.GetID());
		scriptPubKeyAlias = CScript() << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << buyerAliasLatest.vchAlias << buyerAliasLatest.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
		scriptPubKeyAlias += scriptPubKeyAliasOrig;
		foundBuyerKey = true;
		vchLinkAlias = buyerAliasLatest.vchAlias;
	}
	catch(...)
	{
		foundBuyerKey = false;
	}
	bool foundSellerKey = false;
	try
	{
		CKeyID keyID;
		if (!sellerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4620 - " + _("Seller address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4621 - " + _("Private key for seller address is not known"));
		wtxAliasIn = pwalletMain->GetWalletTx(selleraliastx.GetHash());
		if (wtxAliasIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4622 - " + _("Seller alias is not in your wallet"));

		CScript scriptPubKeyAliasOrig = GetScriptForDestination(sellerKey.GetID());
		scriptPubKeyAlias = CScript() << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << sellerAliasLatest.vchAlias << sellerAliasLatest.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
		scriptPubKeyAlias += scriptPubKeyAliasOrig;
		vchLinkAlias = sellerAliasLatest.vchAlias;
		foundSellerKey = true;
	}
	catch(...)
	{
		foundSellerKey = false;
	}
	bool foundArbiterKey = false;
	try
	{
		CKeyID keyID;
		if (!arbiterAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4623 - " + _("Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4624 - " + _("Private key for arbiter address is not known"));
		wtxAliasIn = pwalletMain->GetWalletTx(arbiteraliastx.GetHash());
		if (wtxAliasIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR ERRCODE: 4625 - " + _("Seller alias is not in your wallet"));

		CScript scriptPubKeyAliasOrig = GetScriptForDestination(arbiterKey.GetID());
		scriptPubKeyAlias  = CScript() << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << arbiterAliasLatest.vchAlias << arbiterAliasLatest.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
		scriptPubKeyAlias += scriptPubKeyAliasOrig;
		vchLinkAlias = arbiterAliasLatest.vchAlias;
		foundArbiterKey = true;
	}
	catch(...)
	{
		foundArbiterKey = false;
	}

	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_COMPLETE;
	escrow.nHeight = chainActive.Tip()->nHeight;
	escrow.vchLinkAlias = vchLinkAlias;
	// buyer
	if(foundBuyerKey)
	{
		CFeedback sellerFeedback(FEEDBACKBUYER, FEEDBACKSELLER);
		sellerFeedback.vchFeedback = vchFeedbackPrimary;
		sellerFeedback.nRating = nRatingPrimary;
		sellerFeedback.nHeight = chainActive.Tip()->nHeight;
		CFeedback arbiterFeedback(FEEDBACKBUYER, FEEDBACKARBITER);
		arbiterFeedback.vchFeedback = vchFeedbackSecondary;
		arbiterFeedback.nRating = nRatingSecondary;
		arbiterFeedback.nHeight = chainActive.Tip()->nHeight;
		escrow.feedback.push_back(arbiterFeedback);
		escrow.feedback.push_back(sellerFeedback);
	}
	// seller
	else if(foundSellerKey)
	{
		CFeedback buyerFeedback(FEEDBACKSELLER, FEEDBACKBUYER);
		buyerFeedback.vchFeedback = vchFeedbackPrimary;
		buyerFeedback.nRating = nRatingPrimary;
		buyerFeedback.nHeight = chainActive.Tip()->nHeight;
		CFeedback arbiterFeedback(FEEDBACKSELLER, FEEDBACKARBITER);
		arbiterFeedback.vchFeedback = vchFeedbackSecondary;
		arbiterFeedback.nRating = nRatingSecondary;
		arbiterFeedback.nHeight = chainActive.Tip()->nHeight;
		escrow.feedback.push_back(buyerFeedback);
		escrow.feedback.push_back(arbiterFeedback);
	}
	// arbiter
	else if(foundArbiterKey)
	{
		CFeedback buyerFeedback(FEEDBACKARBITER, FEEDBACKBUYER);
		buyerFeedback.vchFeedback = vchFeedbackPrimary;
		buyerFeedback.nRating = nRatingPrimary;
		buyerFeedback.nHeight = chainActive.Tip()->nHeight;
		CFeedback sellerFeedback(FEEDBACKARBITER, FEEDBACKSELLER);
		sellerFeedback.vchFeedback = vchFeedbackSecondary;
		sellerFeedback.nRating = nRatingSecondary;
		sellerFeedback.nHeight = chainActive.Tip()->nHeight;
		escrow.feedback.push_back(buyerFeedback);
		escrow.feedback.push_back(sellerFeedback);
	}
	else
	{
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4626 - " + _("You must be either the arbiter, buyer or seller to leave feedback on this escrow"));
	}
	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));
	CScript scriptPubKeyBuyer, scriptPubKeySeller,scriptPubKeyArbiter, scriptPubKeyBuyerDestination, scriptPubKeySellerDestination, scriptPubKeyArbiterDestination;
	scriptPubKeyBuyerDestination= GetScriptForDestination(buyerKey.GetID());
	scriptPubKeySellerDestination= GetScriptForDestination(sellerKey.GetID());
	scriptPubKeyArbiterDestination= GetScriptForDestination(arbiterKey.GetID());
	vector<CRecipient> vecSend;
	CRecipient recipientBuyer, recipientSeller, recipientArbiter;
	scriptPubKeyBuyer << CScript::EncodeOP_N(OP_ESCROW_COMPLETE) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
	scriptPubKeyBuyer += scriptPubKeyBuyerDestination;
	scriptPubKeyArbiter << CScript::EncodeOP_N(OP_ESCROW_COMPLETE) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
	scriptPubKeyArbiter += scriptPubKeyArbiterDestination; 
	scriptPubKeySeller << CScript::EncodeOP_N(OP_ESCROW_COMPLETE) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
	scriptPubKeySeller += scriptPubKeySellerDestination;
	CreateRecipient(scriptPubKeySeller, recipientSeller);		
	CreateRecipient(scriptPubKeyBuyer, recipientBuyer);
	CreateRecipient(scriptPubKeyArbiter, recipientArbiter);
	// buyer
	if(foundBuyerKey)
	{
		vecSend.push_back(recipientSeller);
		vecSend.push_back(recipientArbiter);
	}
	// seller
	else if(foundSellerKey)
	{
		vecSend.push_back(recipientBuyer);
		vecSend.push_back(recipientArbiter);
	}
	// arbiter
	else if(foundArbiterKey)
	{
		vecSend.push_back(recipientBuyer);
		vecSend.push_back(recipientSeller);
	}
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxIn=NULL;
	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+recipientSeller.nAmount+recipientArbiter.nAmount+fee.nAmount+aliasRecipient.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxIn);
	UniValue ret(UniValue::VARR);
	ret.push_back(wtx.GetHash().GetHex());
	return ret;
}
UniValue escrowinfo(const UniValue& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error("escrowinfo <guid>\n"
                "Show stored values of a single escrow and its .\n");

    vector<unsigned char> vchEscrow = vchFromValue(params[0]);

    // look for a transaction with this key, also returns
    // an escrow UniValue if it is found
    CTransaction tx;

	vector<CEscrow> vtxPos;

    UniValue oEscrow(UniValue::VOBJ);
    vector<unsigned char> vchValue;

	if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
		  throw runtime_error("failed to read from escrow DB");
	CEscrow ca = vtxPos.back();
	CTransaction offertx;
	COffer offer;
	vector<COffer> offerVtxPos;
	GetTxAndVtxOfOffer(ca.vchOffer, offer, offertx, offerVtxPos, true);
	offer.nHeight = vtxPos.front().nAcceptHeight;
	offer.GetOfferFromList(offerVtxPos);
    string sHeight = strprintf("%llu", ca.nHeight);
    oEscrow.push_back(Pair("escrow", stringFromVch(vchEscrow)));
	string sTime;
	CBlockIndex *pindex = chainActive[ca.nHeight];
	if (pindex) {
		sTime = strprintf("%llu", pindex->nTime);
	}
	int avgBuyerRating, avgSellerRating, avgArbiterRating;
	vector<CFeedback> buyerFeedBacks, sellerFeedBacks, arbiterFeedBacks;
	GetFeedback(buyerFeedBacks, avgBuyerRating, FEEDBACKBUYER, ca.feedback);
	GetFeedback(sellerFeedBacks, avgSellerRating, FEEDBACKSELLER, ca.feedback);
	GetFeedback(arbiterFeedBacks, avgArbiterRating, FEEDBACKARBITER, ca.feedback);

	CAliasIndex theSellerAlias;
	CTransaction aliastx;
	bool isExpired = false;
	vector<CAliasIndex> aliasVtxPos;
	if(GetTxAndVtxOfAlias(ca.vchSellerAlias, theSellerAlias, aliastx, aliasVtxPos, isExpired, true))
	{
		theSellerAlias.nHeight = vtxPos.front().nHeight;
		theSellerAlias.GetAliasFromList(aliasVtxPos);
	}
	oEscrow.push_back(Pair("time", sTime));
	oEscrow.push_back(Pair("seller", stringFromVch(ca.vchSellerAlias)));
	oEscrow.push_back(Pair("arbiter", stringFromVch(ca.vchArbiterAlias)));
	oEscrow.push_back(Pair("buyer", stringFromVch(ca.vchBuyerAlias)));
	oEscrow.push_back(Pair("offer", stringFromVch(ca.vchOffer)));
	oEscrow.push_back(Pair("offertitle", stringFromVch(offer.sTitle)));
	oEscrow.push_back(Pair("quantity", strprintf("%d", ca.nQty)));
	int precision = 2;
	float fEscrowFee = getEscrowFee(offer.vchAliasPeg, offer.sCurrencyCode, vtxPos.front().nAcceptHeight, precision);
	int64_t nEscrowFee = GetEscrowArbiterFee(offer.GetPrice() * ca.nQty, fEscrowFee);
	CAmount nPricePerUnit = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, offer.GetPrice(), vtxPos.front().nAcceptHeight, precision);
	CAmount nFee = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, nEscrowFee, vtxPos.front().nAcceptHeight, precision);
	if(ca.txBTCId.IsNull())
	{
		int sysprecision;
		int nSYSFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("SYS"), vtxPos.front().nAcceptHeight, sysprecision);
		nFee += (nSYSFeePerByte*400);
		oEscrow.push_back(Pair("sysrelayfee",strprintf("%ld", (nSYSFeePerByte*400)))); 
		oEscrow.push_back(Pair("relayfee", strprintf("%.*f SYS", 8, ValueFromAmount(nSYSFeePerByte*400).get_real() )));
	}
	else
	{
		int btcprecision;
		int nBTCFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("BTC"), vtxPos.front().nAcceptHeight, btcprecision);
		nFee += (nBTCFeePerByte*400);
		oEscrow.push_back(Pair("sysrelayfee",strprintf("%ld", (nBTCFeePerByte*400)))); 
		oEscrow.push_back(Pair("relayfee", strprintf("%.*f BTC", 8, ValueFromAmount(nBTCFeePerByte*400).get_real() )));
	}

	oEscrow.push_back(Pair("sysfee", nEscrowFee));
	oEscrow.push_back(Pair("systotal", (offer.GetPrice() * ca.nQty)));
	oEscrow.push_back(Pair("price", strprintf("%.*f", precision, ValueFromAmount(nPricePerUnit).get_real() )));
	oEscrow.push_back(Pair("fee", strprintf("%.*f", 8, ValueFromAmount(nFee).get_real() )));
	
	oEscrow.push_back(Pair("total", strprintf("%.*f", precision, ValueFromAmount(nFee + (nPricePerUnit* ca.nQty)).get_real() )));
	oEscrow.push_back(Pair("currency", stringFromVch(offer.sCurrencyCode)));

				
	string strBTCId = "";
	if(!ca.txBTCId.IsNull())
		strBTCId = ca.txBTCId.GetHex();
	oEscrow.push_back(Pair("btctxid", strBTCId));
	string strRedeemTxIId = "";
	if(!ca.redeemTxId.IsNull())
		strRedeemTxIId = ca.redeemTxId.GetHex();
	oEscrow.push_back(Pair("redeem_txid", strRedeemTxIId));
    oEscrow.push_back(Pair("txid", ca.txHash.GetHex()));
    oEscrow.push_back(Pair("height", sHeight));
	string strMessage = string("");
	if(!DecryptMessage(theSellerAlias.vchPubKey, ca.vchPaymentMessage, strMessage))
		strMessage = _("Encrypted for owner of offer");
	oEscrow.push_back(Pair("pay_message", strMessage));
	int expired_block = ca.nHeight + GetEscrowExpirationDepth();
	int expired = 0;
    if(expired_block < chainActive.Tip()->nHeight && ca.op == OP_ESCROW_COMPLETE)
	{
		expired = 1;
	}  
	oEscrow.push_back(Pair("expired", expired));
	UniValue oBuyerFeedBack(UniValue::VARR);
	for(unsigned int i =0;i<buyerFeedBacks.size();i++)
	{
		UniValue oFeedback(UniValue::VOBJ);
		string sFeedbackTime;
		CBlockIndex *pindex = chainActive[buyerFeedBacks[i].nHeight];
		if (pindex) {
			sFeedbackTime = strprintf("%llu", pindex->nTime);
		}
		oFeedback.push_back(Pair("txid", buyerFeedBacks[i].txHash.GetHex()));
		oFeedback.push_back(Pair("time", sFeedbackTime));
		oFeedback.push_back(Pair("rating", buyerFeedBacks[i].nRating));
		oFeedback.push_back(Pair("feedbackuser", buyerFeedBacks[i].nFeedbackUserFrom));
		oFeedback.push_back(Pair("feedback", stringFromVch(buyerFeedBacks[i].vchFeedback)));
		oBuyerFeedBack.push_back(oFeedback);
	}
	oEscrow.push_back(Pair("buyer_feedback", oBuyerFeedBack));
	oEscrow.push_back(Pair("avg_buyer_rating", avgBuyerRating));
	UniValue oSellerFeedBack(UniValue::VARR);
	for(unsigned int i =0;i<sellerFeedBacks.size();i++)
	{
		UniValue oFeedback(UniValue::VOBJ);
		string sFeedbackTime;
		CBlockIndex *pindex = chainActive[sellerFeedBacks[i].nHeight];
		if (pindex) {
			sFeedbackTime = strprintf("%llu", pindex->nTime);
		}
		oFeedback.push_back(Pair("txid", sellerFeedBacks[i].txHash.GetHex()));
		oFeedback.push_back(Pair("time", sFeedbackTime));
		oFeedback.push_back(Pair("rating", sellerFeedBacks[i].nRating));
		oFeedback.push_back(Pair("feedbackuser", sellerFeedBacks[i].nFeedbackUserFrom));
		oFeedback.push_back(Pair("feedback", stringFromVch(sellerFeedBacks[i].vchFeedback)));
		oSellerFeedBack.push_back(oFeedback);
	}
	oEscrow.push_back(Pair("seller_feedback", oSellerFeedBack));
	oEscrow.push_back(Pair("avg_seller_rating", avgSellerRating));
	UniValue oArbiterFeedBack(UniValue::VARR);
	for(unsigned int i =0;i<arbiterFeedBacks.size();i++)
	{
		UniValue oFeedback(UniValue::VOBJ);
		string sFeedbackTime;
		CBlockIndex *pindex = chainActive[arbiterFeedBacks[i].nHeight];
		if (pindex) {
			sFeedbackTime = strprintf("%llu", pindex->nTime);
		}
		oFeedback.push_back(Pair("txid", arbiterFeedBacks[i].txHash.GetHex()));
		oFeedback.push_back(Pair("time", sFeedbackTime));
		oFeedback.push_back(Pair("rating", arbiterFeedBacks[i].nRating));
		oFeedback.push_back(Pair("feedbackuser", arbiterFeedBacks[i].nFeedbackUserFrom));
		oFeedback.push_back(Pair("feedback", stringFromVch(arbiterFeedBacks[i].vchFeedback)));
		oArbiterFeedBack.push_back(oFeedback);
	}
	oEscrow.push_back(Pair("arbiter_feedback", oArbiterFeedBack));
	oEscrow.push_back(Pair("avg_arbiter_rating", avgArbiterRating));
	unsigned int ratingCount = 0;
	if(avgArbiterRating > 0)
		ratingCount++;
	if(avgSellerRating > 0)
		ratingCount++;
	if(avgBuyerRating > 0)
		ratingCount++;
	if(ratingCount == 0)
		ratingCount = 1;
	float totalAvgRating = roundf((avgArbiterRating+avgSellerRating+avgBuyerRating)/(float)ratingCount);
	oEscrow.push_back(Pair("avg_rating", (int)totalAvgRating));	
	oEscrow.push_back(Pair("avg_rating_count", (int)ratingCount));	
    return oEscrow;
}

UniValue escrowlist(const UniValue& params, bool fHelp) {
    if (fHelp || 1 < params.size())
        throw runtime_error("escrowlist [<escrow>]\n"
                "list my own escrows");
	vector<unsigned char> vchEscrow;

	if (params.size() == 1)
		vchEscrow = vchFromValue(params[0]);
    vector<unsigned char> vchNameUniq;
    if (params.size() == 1)
        vchNameUniq = vchFromValue(params[0]);

    UniValue oRes(UniValue::VARR);
    map< vector<unsigned char>, int > vNamesI;
    map< vector<unsigned char>, UniValue > vNamesO;

    uint256 hash;
    CTransaction tx, dbtx;

    vector<unsigned char> vchValue;
	int pending = 0;
    BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
    {
		int expired_block;
		int expired = 0;
		pending = 0;
        // get txn hash, read txn index
        hash = item.second.GetHash();
		const CWalletTx &wtx = item.second;        // skip non-syscoin txns
		CTransaction tx;
        if (wtx.nVersion != SYSCOIN_TX_VERSION)
            continue;
		vector<vector<unsigned char> > vvch;
		int op, nOut;
		if (!DecodeEscrowTx(wtx, op, nOut, vvch) || !IsEscrowOp(op))
			continue;
		vchEscrow = vvch[0];
		vector<CEscrow> vtxPos;
		CEscrow escrow;
		bool escrowRelease = false;
		bool escrowRefund = false;
		if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
		{
			pending = 1;
			escrow = CEscrow(wtx);
		}
		else
		{
			escrow = vtxPos.back();
			CTransaction tx;
			if (!GetSyscoinTransaction(escrow.nHeight, escrow.txHash, tx, Params().GetConsensus()))
			{
				pending = 1;
			}
			else{
				if (!DecodeEscrowTx(tx, op, nOut, vvch) || !IsEscrowOp(op))
					continue;
			}
			if(escrow.op == OP_ESCROW_COMPLETE)
			{
				for(int i = vtxPos.size() - 1; i >= 0;i--)
				{
					if(vtxPos[i].op == OP_ESCROW_RELEASE)
					{
						escrowRelease = true;
						break;
					}
					else if(vtxPos[i].op == OP_ESCROW_REFUND)
					{
						escrowRefund = true;
						break;
					}
				}
			}
		}
		int nHeight;
		COffer offer;
		CTransaction offertx;
		vector<COffer> offerVtxPos;
		GetTxAndVtxOfOffer(escrow.vchOffer, offer, offertx, offerVtxPos, true);
		if(vtxPos.empty())
			nHeight = escrow.nAcceptHeight;
		else
			nHeight = vtxPos.front().nAcceptHeight;
		offer.nHeight = nHeight;
		offer.GetOfferFromList(offerVtxPos);
		// skip this escrow if it doesn't match the given filter value
		if (vchNameUniq.size() > 0 && vchNameUniq != vchEscrow)
			continue;
		// get last active name only
		if (vNamesI.find(vchEscrow) != vNamesI.end() && (escrow.nHeight <= vNamesI[vchEscrow] || vNamesI[vchEscrow] < 0))
			continue;

        // build the output
        UniValue oName(UniValue::VOBJ);
        oName.push_back(Pair("escrow", stringFromVch(vchEscrow)));
		string sTime;
		CBlockIndex *pindex = chainActive[escrow.nHeight];
		if (pindex) {
			sTime = strprintf("%llu", pindex->nTime);
		}
		int avgBuyerRating, avgSellerRating, avgArbiterRating;
		vector<CFeedback> buyerFeedBacks, sellerFeedBacks, arbiterFeedBacks;
		GetFeedback(buyerFeedBacks, avgBuyerRating, FEEDBACKBUYER, escrow.feedback);
		GetFeedback(sellerFeedBacks, avgSellerRating, FEEDBACKSELLER, escrow.feedback);
		GetFeedback(arbiterFeedBacks, avgArbiterRating, FEEDBACKARBITER, escrow.feedback);



		oName.push_back(Pair("time", sTime));
		oName.push_back(Pair("seller", stringFromVch(escrow.vchSellerAlias)));
		oName.push_back(Pair("arbiter", stringFromVch(escrow.vchArbiterAlias)));
		oName.push_back(Pair("buyer", stringFromVch(escrow.vchBuyerAlias)));
		oName.push_back(Pair("offer", stringFromVch(escrow.vchOffer)));
		oName.push_back(Pair("offertitle", stringFromVch(offer.sTitle)));
		int precision = 2;
		float fEscrowFee = getEscrowFee(offer.vchAliasPeg, offer.sCurrencyCode, nHeight, precision);
		int64_t nEscrowFee = GetEscrowArbiterFee(offer.GetPrice() * escrow.nQty, fEscrowFee);
		CAmount nPricePerUnit = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, offer.GetPrice(), nHeight, precision);
		CAmount nFee = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, nEscrowFee, nHeight, precision);
		if(escrow.txBTCId.IsNull())
		{
			int sysprecision;
			int nSYSFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("SYS"), nHeight, sysprecision);
			nFee += (nSYSFeePerByte*400);
			oName.push_back(Pair("sysrelayfee",strprintf("%ld", (nSYSFeePerByte*400))));
			oName.push_back(Pair("relayfee", strprintf("%.*f SYS", 8, ValueFromAmount(nSYSFeePerByte*400).get_real() )));
		}
		else
		{
			int btcprecision;
			int nBTCFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("BTC"), nHeight, btcprecision);
			nFee += (nBTCFeePerByte*400);
			oName.push_back(Pair("sysrelayfee",strprintf("%ld", (nBTCFeePerByte*400)))); 
			oName.push_back(Pair("relayfee", strprintf("%.*f BTC", 8, ValueFromAmount(nBTCFeePerByte*400).get_real() )));
		}
		oName.push_back(Pair("sysfee", nEscrowFee));
		oName.push_back(Pair("systotal", (offer.GetPrice() * escrow.nQty)));
		oName.push_back(Pair("price", strprintf("%.*f", precision, ValueFromAmount(nPricePerUnit).get_real() )));
		oName.push_back(Pair("fee", strprintf("%.*f", 8, ValueFromAmount(nFee).get_real() )));
		oName.push_back(Pair("total", strprintf("%.*f", precision, ValueFromAmount(nFee + (nPricePerUnit* escrow.nQty)).get_real() )));
		oName.push_back(Pair("currency", stringFromVch(offer.sCurrencyCode)));
		string strBTCId = "";
		if(!escrow.txBTCId.IsNull())
			strBTCId = escrow.txBTCId.GetHex();
		oName.push_back(Pair("btctxid", strBTCId));
		string strRedeemTxIId = "";
		if(!escrow.redeemTxId.IsNull())
			strRedeemTxIId = escrow.redeemTxId.GetHex();
		oName.push_back(Pair("redeem_txid", strRedeemTxIId));
		expired_block = escrow.nHeight + GetEscrowExpirationDepth();
        if(expired_block < chainActive.Tip()->nHeight && escrow.op == OP_ESCROW_COMPLETE)
		{
			expired = 1;
		} 
	
		string status = "unknown";
		if(pending == 0)
		{
			if(op == OP_ESCROW_ACTIVATE)
				status = "in escrow";
			else if(op == OP_ESCROW_RELEASE && vvch[1] == vchFromString("0"))
				status = "escrow released";
			else if(op == OP_ESCROW_RELEASE && vvch[1] == vchFromString("1"))
				status = "escrow release complete";
			else if(op == OP_ESCROW_COMPLETE && escrowRelease)
				status = "escrow release complete";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("0"))
				status = "escrow refunded";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("1"))
				status = "escrow refund complete";
			else if(op == OP_ESCROW_COMPLETE && escrowRefund)
				status = "escrow refund complete";
		}
		else
			status = "pending";
		UniValue oBuyerFeedBack(UniValue::VARR);
		for(unsigned int i =0;i<buyerFeedBacks.size();i++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[buyerFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("txid", buyerFeedBacks[i].txHash.GetHex()));
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", buyerFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", buyerFeedBacks[i].nFeedbackUserFrom));
			oFeedback.push_back(Pair("feedback", stringFromVch(buyerFeedBacks[i].vchFeedback)));
			oBuyerFeedBack.push_back(oFeedback);
		}
		oName.push_back(Pair("buyer_feedback", oBuyerFeedBack));
		oName.push_back(Pair("avg_buyer_rating", avgBuyerRating));
		UniValue oSellerFeedBack(UniValue::VARR);
		for(unsigned int i =0;i<sellerFeedBacks.size();i++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[sellerFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("txid", sellerFeedBacks[i].txHash.GetHex()));
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", sellerFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", sellerFeedBacks[i].nFeedbackUserFrom));
			oFeedback.push_back(Pair("feedback", stringFromVch(sellerFeedBacks[i].vchFeedback)));
			oSellerFeedBack.push_back(oFeedback);
		}
		oName.push_back(Pair("seller_feedback", oSellerFeedBack));
		oName.push_back(Pair("avg_seller_rating", avgSellerRating));
		UniValue oArbiterFeedBack(UniValue::VARR);
		for(unsigned int i =0;i<arbiterFeedBacks.size();i++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[arbiterFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("txid", arbiterFeedBacks[i].txHash.GetHex()));
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", arbiterFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", arbiterFeedBacks[i].nFeedbackUserFrom));
			oFeedback.push_back(Pair("feedback", stringFromVch(arbiterFeedBacks[i].vchFeedback)));
			oArbiterFeedBack.push_back(oFeedback);
		}
		oName.push_back(Pair("arbiter_feedback", oArbiterFeedBack));
		oName.push_back(Pair("avg_arbiter_rating", avgArbiterRating));
		unsigned int ratingCount = 0;
		if(avgArbiterRating > 0)
			ratingCount++;
		if(avgSellerRating > 0)
			ratingCount++;
		if(avgBuyerRating > 0)
			ratingCount++;
		if(ratingCount == 0)
			ratingCount = 1;
		float totalAvgRating = roundf((avgArbiterRating+avgSellerRating+avgBuyerRating)/(float)ratingCount);
		oName.push_back(Pair("avg_rating", (int)totalAvgRating));	
		oName.push_back(Pair("avg_rating_count", (int)ratingCount));	
		oName.push_back(Pair("status", status));
		oName.push_back(Pair("expired", expired));
 
		vNamesI[vchEscrow] = escrow.nHeight;
		vNamesO[vchEscrow] = oName;	
		
    
	}
    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, UniValue)& item, vNamesO)
        oRes.push_back(item.second);
    return oRes;
}


UniValue escrowhistory(const UniValue& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error("escrowhistory <escrow>\n"
                "List all stored values of an escrow.\n");

    UniValue oRes(UniValue::VARR);
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);
    string escrow = stringFromVch(vchEscrow);

    {
        vector<CEscrow> vtxPos;
        if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
            throw runtime_error("failed to read from escrow DB");

        CEscrow txPos2;
        uint256 txHash;
        uint256 blockHash;
        BOOST_FOREACH(txPos2, vtxPos) {

            txHash = txPos2.txHash;
			CTransaction tx;
			if (!GetSyscoinTransaction(txPos2.nHeight, txHash, tx, Params().GetConsensus())) {
				error("could not read txpos");
				continue;
			}
			COffer offer;
			CTransaction offertx;
			vector<COffer> offerVtxPos;
			GetTxAndVtxOfOffer(txPos2.vchOffer, offer, offertx, offerVtxPos, true);
			offer.nHeight = vtxPos.front().nAcceptHeight;
			offer.GetOfferFromList(offerVtxPos);				
            // decode txn, skip non-alias txns
            vector<vector<unsigned char> > vvch;
            int op, nOut;
            if (!DecodeEscrowTx(tx, op, nOut, vvch) 
            	|| !IsEscrowOp(op) )
                continue;
			bool escrowRelease = false;
			bool escrowRefund = false;
			if(txPos2.op == OP_ESCROW_COMPLETE)
			{
				for(int i = vtxPos.size() - 1; i >= 0;i--)
				{
					if(vtxPos[i].op == OP_ESCROW_RELEASE)
					{
						escrowRelease = true;
						break;
					}
					else if(vtxPos[i].op == OP_ESCROW_REFUND)
					{
						escrowRefund = true;
						break;
					}
				}
			}
			int expired = 0;
            UniValue oEscrow(UniValue::VOBJ);
            uint64_t nHeight;
			nHeight = txPos2.nHeight;
			oEscrow.push_back(Pair("escrow", escrow));
			string opName = escrowFromOp(op);
			oEscrow.push_back(Pair("escrowtype", opName));

			oEscrow.push_back(Pair("txid", tx.GetHash().GetHex()));
			oEscrow.push_back(Pair("seller", stringFromVch(txPos2.vchSellerAlias)));
			oEscrow.push_back(Pair("arbiter", stringFromVch(txPos2.vchArbiterAlias)));
			oEscrow.push_back(Pair("buyer", stringFromVch(txPos2.vchBuyerAlias)));
			oEscrow.push_back(Pair("offer", stringFromVch(txPos2.vchOffer)));
			oEscrow.push_back(Pair("offertitle", stringFromVch(offer.sTitle)));
			int precision = 2;
			float fEscrowFee = getEscrowFee(offer.vchAliasPeg, offer.sCurrencyCode, vtxPos.front().nAcceptHeight, precision);
			int64_t nEscrowFee = GetEscrowArbiterFee(offer.GetPrice() * txPos2.nQty, fEscrowFee);
			CAmount nPricePerUnit = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, offer.GetPrice(),  vtxPos.front().nAcceptHeight, precision);
			CAmount nFee = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, nEscrowFee, vtxPos.front().nAcceptHeight, precision);
			if(txPos2.txBTCId.IsNull())
			{
				int sysprecision;
				int nSYSFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("SYS"), vtxPos.front().nAcceptHeight, sysprecision);
				nFee += (nSYSFeePerByte*400);
				oEscrow.push_back(Pair("sysrelayfee",strprintf("%ld", nSYSFeePerByte*400))); 
				oEscrow.push_back(Pair("relayfee", strprintf("%.*f SYS", 8, ValueFromAmount(nSYSFeePerByte*400).get_real() )));
			}
			else
			{
				int btcprecision;
				int nBTCFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("BTC"), vtxPos.front().nAcceptHeight, btcprecision);
				nFee += (nBTCFeePerByte*400);
				oEscrow.push_back(Pair("sysrelayfee",strprintf("%ld", nBTCFeePerByte*400)));
				oEscrow.push_back(Pair("relayfee", strprintf("%.*f BTC", 8, ValueFromAmount(nBTCFeePerByte*400).get_real() )));
			}
			oEscrow.push_back(Pair("sysfee", nEscrowFee));
			oEscrow.push_back(Pair("systotal", (offer.GetPrice() * txPos2.nQty)));
			oEscrow.push_back(Pair("price", strprintf("%.*f", precision, ValueFromAmount(nPricePerUnit).get_real() )));
			oEscrow.push_back(Pair("fee", strprintf("%.*f", 8, ValueFromAmount(nFee).get_real() )));
			oEscrow.push_back(Pair("total", strprintf("%.*f", precision, ValueFromAmount(nFee + (nPricePerUnit* txPos2.nQty)).get_real() )));
			oEscrow.push_back(Pair("currency", stringFromVch(offer.sCurrencyCode)));
			string strBTCId = "";
			if(!txPos2.txBTCId.IsNull())
				strBTCId = txPos2.txBTCId.GetHex();
			oEscrow.push_back(Pair("btctxid", strBTCId));
			string strRedeemTxIId = "";
			if(!txPos2.redeemTxId.IsNull())
				strRedeemTxIId = txPos2.redeemTxId.GetHex();
			oEscrow.push_back(Pair("redeem_txid", strRedeemTxIId));
			if(nHeight + GetEscrowExpirationDepth() - chainActive.Tip()->nHeight <= 0  && txPos2.op == OP_ESCROW_COMPLETE)
			{
				expired = 1;
			}  
	
			string status = "unknown";

			if(op == OP_ESCROW_ACTIVATE)
				status = "in escrow";
			else if(op == OP_ESCROW_RELEASE && vvch[1] == vchFromString("0"))
				status = "escrow released";
			else if(op == OP_ESCROW_RELEASE && vvch[1] == vchFromString("1"))
				status = "escrow release complete";
			else if(op == OP_ESCROW_COMPLETE && escrowRelease)
				status = "escrow release complete";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("0"))
				status = "escrow refunded";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("1"))
				status = "escrow refund complete";
			else if(op == OP_ESCROW_COMPLETE && escrowRefund)
				status = "escrow refund complete";

			oEscrow.push_back(Pair("status", status));
			oEscrow.push_back(Pair("expired", expired));
			oEscrow.push_back(Pair("height", strprintf("%d", nHeight)));
			oRes.push_back(oEscrow);
        }
        
    }
    return oRes;
}
UniValue escrowfilter(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() > 2)
		throw runtime_error(
				"escrowfilter [[[[[regexp]] from=0]}\n"
						"scan and filter escrows\n"
						"[regexp] : apply [regexp] on escrows, empty means all escrows\n"
						"[from] : show results from this GUID [from], 0 means first.\n"
						"[escrowfilter] : shows all escrows that are safe to display (not on the ban list)\n"
						"escrowfilter \"\" 5 # list escrows updated in last 5 blocks\n"
						"escrowfilter \"^escrow\" # list all excrows starting with \"escrow\"\n"
						"escrowfilter 36000 0 0 stat # display stats (number of escrows) on active escrows\n");

	vector<unsigned char> vchEscrow;
	string strRegexp;

	if (params.size() > 0)
		strRegexp = params[0].get_str();

	if (params.size() > 1)
		vchEscrow = vchFromValue(params[1]);

	UniValue oRes(UniValue::VARR);

   
    vector<pair<vector<unsigned char>, CEscrow> > escrowScan;
    if (!pescrowdb->ScanEscrows(vchEscrow, strRegexp, 25, escrowScan))
        throw runtime_error("scan failed");
    pair<vector<unsigned char>, CEscrow> pairScan;
    BOOST_FOREACH(pairScan, escrowScan) {
		const CEscrow &txEscrow = pairScan.second;  
		const string &escrow = stringFromVch(pairScan.first);
		vector<COffer> offerVtxPos;
		vector<CEscrow> vtxPos;
		COffer offer;
        int nHeight = txEscrow.nHeight;
		CTransaction tx;
		if (!GetSyscoinTransaction(nHeight, txEscrow.txHash, tx, Params().GetConsensus())) {
			continue;
		}
        // decode txn, skip non-alias txns
        vector<vector<unsigned char> > vvch;
        int op, nOut;
        if (!DecodeEscrowTx(tx, op, nOut, vvch) 
        	|| !IsEscrowOp(op) )
            continue; 
		int avgBuyerRating, avgSellerRating, avgArbiterRating;
		if (!pescrowdb->ReadEscrow(pairScan.first, vtxPos) || vtxPos.empty())
			continue;
		bool escrowRelease = false;
		bool escrowRefund = false;
		if(txEscrow.op == OP_ESCROW_COMPLETE)
		{
			for(int i = vtxPos.size() - 1; i >= 0;i--)
			{
				if(vtxPos[i].op == OP_ESCROW_RELEASE)
				{
					escrowRelease = true;
					break;
				}
				else if(vtxPos[i].op == OP_ESCROW_REFUND)
				{
					escrowRefund = true;
					break;
				}
			}
		}
		if (pofferdb->ReadOffer(txEscrow.vchOffer, offerVtxPos) && !offerVtxPos.empty())
		{
			offer.nHeight = vtxPos.front().nAcceptHeight;
			offer.GetOfferFromList(offerVtxPos);
		}
		vector<CFeedback> buyerFeedBacks, sellerFeedBacks, arbiterFeedBacks;
		GetFeedback(buyerFeedBacks, avgBuyerRating, FEEDBACKBUYER, txEscrow.feedback);
		GetFeedback(sellerFeedBacks, avgSellerRating, FEEDBACKSELLER, txEscrow.feedback);
		GetFeedback(arbiterFeedBacks, avgArbiterRating, FEEDBACKARBITER, txEscrow.feedback);
		
        UniValue oEscrow(UniValue::VOBJ);
        oEscrow.push_back(Pair("escrow", escrow));
		string sTime;
		CBlockIndex *pindex = chainActive[txEscrow.nHeight];
		if (pindex) {
			sTime = strprintf("%llu", pindex->nTime);
		}
		oEscrow.push_back(Pair("time", sTime));
		
		oEscrow.push_back(Pair("seller", stringFromVch(txEscrow.vchSellerAlias)));
		oEscrow.push_back(Pair("arbiter", stringFromVch(txEscrow.vchArbiterAlias)));
		oEscrow.push_back(Pair("buyer", stringFromVch(txEscrow.vchBuyerAlias)));
		oEscrow.push_back(Pair("offer", stringFromVch(txEscrow.vchOffer)));
		oEscrow.push_back(Pair("offertitle", stringFromVch(offer.sTitle)));
	
		string status = "unknown";

		if(op == OP_ESCROW_ACTIVATE)
			status = "in escrow";
		else if(op == OP_ESCROW_RELEASE && vvch[1] == vchFromString("0"))
			status = "escrow released";
		else if(op == OP_ESCROW_RELEASE && vvch[1] == vchFromString("1"))
			status = "escrow release complete";
		else if(op == OP_ESCROW_COMPLETE && escrowRelease)
			status = "escrow release complete";
		else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("0"))
			status = "escrow refunded";
		else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("1"))
			status = "escrow refund complete";
		else if(op == OP_ESCROW_COMPLETE && escrowRefund)
			status = "escrow refund complete";
		

		oEscrow.push_back(Pair("status", status));
		int precision = 2;
		float fEscrowFee = getEscrowFee(offer.vchAliasPeg, offer.sCurrencyCode, vtxPos.front().nAcceptHeight, precision);
		int64_t nEscrowFee = GetEscrowArbiterFee(offer.GetPrice() * txEscrow.nQty, fEscrowFee);
		CAmount nPricePerUnit = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, offer.GetPrice(),  vtxPos.front().nAcceptHeight, precision);
		CAmount nFee = convertSyscoinToCurrencyCode(offer.vchAliasPeg, offer.sCurrencyCode, nEscrowFee, vtxPos.front().nAcceptHeight, precision);
		if(txEscrow.txBTCId.IsNull())
		{
			int sysprecision;
			int nSYSFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("SYS"), vtxPos.front().nAcceptHeight, sysprecision);
			nFee += (nSYSFeePerByte*400);
			oEscrow.push_back(Pair("sysrelayfee",strprintf("%ld", nSYSFeePerByte*400))); 
			oEscrow.push_back(Pair("relayfee", strprintf("%.*f SYS", 8, ValueFromAmount(nSYSFeePerByte*400).get_real() )));
		}
		else
		{
			int btcprecision;
			int nBTCFeePerByte = getFeePerByte(offer.vchAliasPeg, vchFromString("BTC"), vtxPos.front().nAcceptHeight, btcprecision);
			nFee += (nBTCFeePerByte*400);
			oEscrow.push_back(Pair("sysrelayfee",strprintf("%ld", nBTCFeePerByte*400))); 
			oEscrow.push_back(Pair("relayfee", strprintf("%.*f BTC", 8, ValueFromAmount(nBTCFeePerByte*400).get_real() )));
		}
		oEscrow.push_back(Pair("sysfee", nEscrowFee));
		oEscrow.push_back(Pair("systotal", (offer.GetPrice() * txEscrow.nQty)));
		oEscrow.push_back(Pair("price", strprintf("%.*f", precision, ValueFromAmount(nPricePerUnit).get_real() )));
		oEscrow.push_back(Pair("fee", strprintf("%.*f", 8, ValueFromAmount(nFee).get_real() )));
		oEscrow.push_back(Pair("total", strprintf("%.*f", precision, ValueFromAmount(nFee + (nPricePerUnit* txEscrow.nQty)).get_real() )));
		oEscrow.push_back(Pair("currency", stringFromVch(offer.sCurrencyCode)));
		string strBTCId = "";
		if(!txEscrow.txBTCId.IsNull())
			strBTCId = txEscrow.txBTCId.GetHex();
		oEscrow.push_back(Pair("btctxid", strBTCId));
		string strRedeemTxIId = "";
		if(!txEscrow.redeemTxId.IsNull())
			strRedeemTxIId = txEscrow.redeemTxId.GetHex();
		oEscrow.push_back(Pair("redeem_txid", strRedeemTxIId));

		UniValue oBuyerFeedBack(UniValue::VARR);
		for(unsigned int i =0;i<buyerFeedBacks.size();i++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[buyerFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", buyerFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", buyerFeedBacks[i].nFeedbackUserFrom));
			oFeedback.push_back(Pair("feedback", stringFromVch(buyerFeedBacks[i].vchFeedback)));
			oBuyerFeedBack.push_back(oFeedback);
		}
		oEscrow.push_back(Pair("buyer_feedback", oBuyerFeedBack));
		oEscrow.push_back(Pair("avg_buyer_rating", avgBuyerRating));
		UniValue oSellerFeedBack(UniValue::VARR);
		for(unsigned int i =0;i<sellerFeedBacks.size();i++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[sellerFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", sellerFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", sellerFeedBacks[i].nFeedbackUserFrom));
			oFeedback.push_back(Pair("feedback", stringFromVch(sellerFeedBacks[i].vchFeedback)));
			oSellerFeedBack.push_back(oFeedback);
		}
		oEscrow.push_back(Pair("seller_feedback", oSellerFeedBack));
		oEscrow.push_back(Pair("avg_seller_rating", avgSellerRating));
		UniValue oArbiterFeedBack(UniValue::VARR);
		for(unsigned int i =0;i<arbiterFeedBacks.size();i++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[arbiterFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", arbiterFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", arbiterFeedBacks[i].nFeedbackUserFrom));
			oFeedback.push_back(Pair("feedback", stringFromVch(arbiterFeedBacks[i].vchFeedback)));
			oArbiterFeedBack.push_back(oFeedback);
		}
		oEscrow.push_back(Pair("arbiter_feedback", oArbiterFeedBack));
		oEscrow.push_back(Pair("avg_arbiter_rating", avgArbiterRating));
		unsigned int ratingCount = 0;
		if(avgArbiterRating > 0)
			ratingCount++;
		if(avgSellerRating > 0)
			ratingCount++;
		if(avgBuyerRating > 0)
			ratingCount++;
		if(ratingCount == 0)
			ratingCount = 1;
		float totalAvgRating = roundf((avgArbiterRating+avgSellerRating+avgBuyerRating)/(float)ratingCount);
		oEscrow.push_back(Pair("avg_rating", (int)totalAvgRating));	
		oEscrow.push_back(Pair("avg_rating_count", (int)ratingCount));	
        oRes.push_back(oEscrow);
    }


	return oRes;
}