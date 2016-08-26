#include "escrow.h"
#include "offer.h"
#include "alias.h"
#include "cert.h"
#include "init.h"
#include "main.h"
#include "util.h"
#include "base58.h"
#include "rpcserver.h"
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
extern void SendMoneySyscoin(const vector<CRecipient> &vecSend, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew, const CWalletTx* wtxInOffer=NULL, const CWalletTx* wtxInCert=NULL, const CWalletTx* wtxInAlias=NULL, const CWalletTx* wtxInEscrow=NULL, bool syscoinTx=true, string justcheck="0");
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
// 0.05% fee on escrow value for arbiter
int64_t GetEscrowArbiterFee(int64_t escrowValue) {

	int64_t nFee = escrowValue*0.005;
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
	if(!GetSyscoinData(tx, vchData, vchHash))
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
bool CheckEscrowInputs(const CTransaction &tx, int op, int nOut, const vector<vector<unsigned char> > &vvchArgs, const CCoinsViewCache &inputs, bool fJustCheck, int nHeight, string &errorMessage, const CBlock* block, bool dontaddtodb, string justcheck) {
	if(!IsSys21Fork(nHeight))
		return true;	
	if (tx.IsCoinBase())
		return true;
	const COutPoint *prevOutput = NULL;
	CCoins prevCoins;
	int prevOp = 0;
	bool foundEscrow = false;
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
	if(GetSyscoinData(tx, vchData, vchHash) && !theEscrow.UnserializeFromData(vchData, vchHash))
	{
		theEscrow.SetNull();
	}

	vector<vector<unsigned char> > vvchPrevArgs;
	if(fJustCheck)
	{
		
		if(vvchArgs.size() != 3)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4001 - " + _("Escrow arguments incorrect size");
			return error(errorMessage.c_str());
		}
		if(!theEscrow.IsNull())
		{
			if(vchHash != vvchArgs[2])
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4002 - " + _("Hash provided doesn't match the calculated hash the data");
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
			if(foundEscrow)
				break;

			if (!foundEscrow && IsEscrowOp(pop)) {
				foundEscrow = true; 
				prevOp = pop;
				vvchPrevArgs = vvch;
			}
		}
	}

	vector<COffer> myVtxPos;
	CAliasIndex alias;
	CTransaction aliasTx;
    COffer theOffer;
	string retError = "";
	// null usually when pruned or when accept is done (in which case we skip this return and continue on so future feedbacks can be done)
	if(theEscrow.IsNull() && !(op == OP_ESCROW_COMPLETE && vvchArgs[1] == vchFromString("0")))
	{
		if(fDebug)
			LogPrintf("SYSCOIN_ESCROW_CONSENSUS_ERROR: Null escrow, skipping...\n");	
		return true;
	}
 
	CTransaction txOffer;
	COffer dbOffer;
	if(fJustCheck)
	{
		if (vvchArgs[0].size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4003 - " + _("Escrow guid too big");
			return error(errorMessage.c_str());
		}
		if(theEscrow.vchRedeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4004 - " + _("Escrow redeem script too long");
			return error(errorMessage.c_str());
		}
		if(theEscrow.buyerFeedback.vchFeedback.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4005 - " + _("Buyer feedback too long");
			return error(errorMessage.c_str());
		}
		if(theEscrow.sellerFeedback.vchFeedback.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4006 - " + _("Seller feedback too long");
			return error(errorMessage.c_str());
		}
		if(theEscrow.arbiterFeedback.vchFeedback.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4007 - " + _("Arbiter feedback too long");
			return error(errorMessage.c_str());
		}
		if(theEscrow.vchOffer.size() > MAX_ID_LENGTH)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4008 - " + _("Escrow offer guid too long");
			return error(errorMessage.c_str());
		}

		if(theEscrow.rawTx.size() > MAX_STANDARD_TX_SIZE)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4009 - " + _("Escrow raw transaction too big");
			return error(errorMessage.c_str());
		}
		if(theEscrow.vchOfferAcceptLink.size() > 0)
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4010 - " + _("Escrow offeraccept guid not allowed");
			return error(errorMessage.c_str());
		}
		if(!theEscrow.vchEscrow.empty() && theEscrow.vchEscrow != vvchArgs[0])
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4011 - " + _("Escrow guid in data output doesn't match guid in transaction");
			return error(errorMessage.c_str());
		}
		switch (op) {
			case OP_ESCROW_ACTIVATE:
				if(theEscrow.op != OP_ESCROW_ACTIVATE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4012 - " + _("Invalid op, should be escrow activate");
					return error(errorMessage.c_str());
				}
				if (theEscrow.vchEscrow != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4013 - " + _("Guid mismatch");
					return error(errorMessage.c_str());
				}
				if(!theEscrow.buyerFeedback.IsNull() || !theEscrow.sellerFeedback.IsNull() || !theEscrow.arbiterFeedback.IsNull())
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4014 - " + _("Cannot leave feedback in escrow activation");
					return error(errorMessage.c_str());
				}				
				break;
			case OP_ESCROW_RELEASE:
				if(prevOp != OP_ESCROW_ACTIVATE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4015 - " + _("Can only release an activated escrow");
					return error(errorMessage.c_str());
				}	
				// Check input
				if (vvchPrevArgs[0] != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4016 - " + _("Escrow input guid mismatch");
					return error(errorMessage.c_str());
				}	
				if(!theEscrow.buyerFeedback.IsNull() || !theEscrow.sellerFeedback.IsNull() || !theEscrow.arbiterFeedback.IsNull())
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4017 - " + _("Cannot leave feedback in escrow release");
					return error(errorMessage.c_str());
				}
				if(theEscrow.op != OP_ESCROW_RELEASE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4018 - " + _("Invalid op, should be escrow release");
					return error(errorMessage.c_str());
				}
				if (theEscrow.vchEscrow != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4020 - " + _("Guid mismatch");
					return error(errorMessage.c_str());
				}
				break;
			case OP_ESCROW_COMPLETE:
				// Check input
				if (vvchArgs[1].size() > 1)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4021 - " + _("Escrow complete status too large");
					return error(errorMessage.c_str());
				}
				if (vvchPrevArgs[0] != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4022 - " + _("Escrow input guid mismatch");
					return error(errorMessage.c_str());
				}
				if (!theEscrow.IsNull() && theEscrow.op != OP_ESCROW_COMPLETE)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4023 - " + _("Invalid op, should be escrow complete");
					return error(errorMessage.c_str());
				}
				if (!theEscrow.IsNull() && theEscrow.vchEscrow != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4024 - " + _("Guid mismatch");
					return error(errorMessage.c_str());
				}
				if(vvchArgs[1] == vchFromString("1"))
				{
					if(prevOp != OP_ESCROW_COMPLETE && prevOp != OP_ESCROW_REFUND)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4025 - " + _("Can only leave feedback for a completed escrow");
						return error(errorMessage.c_str());
					}
					if(theEscrow.buyerFeedback.IsNull() && theEscrow.sellerFeedback.IsNull() && theEscrow.arbiterFeedback.IsNull())
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4026 - " + _("Cannot leave empty feedback");
						return error(errorMessage.c_str());
					}
					if(prevOp == OP_ESCROW_COMPLETE && vvchPrevArgs[1] == vchFromString("1") && theEscrow.buyerFeedback.vchFeedback.size() <= 0 && theEscrow.sellerFeedback.vchFeedback.size() <= 0 && theEscrow.arbiterFeedback.vchFeedback.size() <= 0)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4027 - " + _("Feedback reply must leave a message");
						return error(errorMessage.c_str());
					}
					if(theEscrow.buyerFeedback.nRating < 0 || theEscrow.buyerFeedback.nRating > 5)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4028 - " + _("Invalid buyer rating, must be less than or equal to 5 and greater than or equal to 0");
						return error(errorMessage.c_str());
					}
					if(theEscrow.sellerFeedback.nRating < 0 || theEscrow.sellerFeedback.nRating > 5)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4029 - " + _("Invalid seller rating, must be less than or equal to 5 and greater than or equal to 0");
						return error(errorMessage.c_str());
					}
					if(theEscrow.arbiterFeedback.nRating < 0 || theEscrow.arbiterFeedback.nRating > 5)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4030 - " + _("Invalid arbiter rating, must be less than or equal to 5 and greater than or equal to 0");
						return error(errorMessage.c_str());
					}
				}		
				else
				{
					if(prevOp != OP_ESCROW_RELEASE && justcheck != "1")
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4031 - " + _("Can only complete a released escrow");
						return error(errorMessage.c_str());
					}

					if(!theEscrow.buyerFeedback.IsNull() || !theEscrow.sellerFeedback.IsNull() || !theEscrow.arbiterFeedback.IsNull())
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4032 - " + _("Cannot leave feedback in escrow complete");
						return error(errorMessage.c_str());
					}
				}
				break;			
			case OP_ESCROW_REFUND:
				if (vvchArgs[1].size() > 1)
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4033 - " + _("Escrow refund status too large");
					return error(errorMessage.c_str());
				}
				if (vvchPrevArgs[0] != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4034 - " + _("Escrow input guid mismatch");
					return error(errorMessage.c_str());
				}
				if (theEscrow.vchEscrow != vvchArgs[0])
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4035 - " + _("Guid mismatch");
					return error(errorMessage.c_str());
				}
				if(vvchArgs[1] == vchFromString("1"))
				{
					if(prevOp != OP_ESCROW_REFUND)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4036 - " + _("Can only complete refund on a refunded escrow");
						return error(errorMessage.c_str());
					}
					if(theEscrow.op != OP_ESCROW_COMPLETE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4037 - " + _("Invalid op, should be escrow complete");
						return error(errorMessage.c_str());
					}

				}
				else
				{
					if(prevOp != OP_ESCROW_ACTIVATE)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4038 - " + _("Can only refund an activated escrow");
						return error(errorMessage.c_str());
					}
					if(theEscrow.op != OP_ESCROW_REFUND)
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4039 - " + _("Invalid op, should be escrow refund");
						return error(errorMessage.c_str());
					}
				}
				// Check input
				if(!theEscrow.buyerFeedback.IsNull() || !theEscrow.sellerFeedback.IsNull() || !theEscrow.arbiterFeedback.IsNull())
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4040 - " + _("Cannot leave feedback in escrow refund");
					return error(errorMessage.c_str());
				}
				


				break;
			default:
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4041 - " + _("Escrow transaction has unknown op");
				return error(errorMessage.c_str());
		}
	}



    if (!fJustCheck ) {
		if(op == OP_ESCROW_ACTIVATE) 
		{
			if(!GetTxOfAlias(theEscrow.vchBuyerAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4042 - " + _("Cannot find buyer alias. It may be expired");
				return true;
			}
			if(!GetTxOfAlias(theEscrow.vchSellerAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4043 - " + _("Cannot find seller alias. It may be expired");
				return true;
			}	
			if(!GetTxOfAlias(theEscrow.vchArbiterAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4044 - " + _("Cannot find arbiter alias. It may be expired");
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
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4045 - " + _("Failed to read from escrow DB");
				return true;
			}
			
			// make sure we have found this escrow in db
			if(!vtxPos.empty())
			{			
				// these are the only settings allowed to change outside of activate
				if(!serializedEscrow.rawTx.empty())
					theEscrow.rawTx = serializedEscrow.rawTx;
				theEscrow.op = serializedEscrow.op;
				if(op == OP_ESCROW_REFUND && vvchArgs[1] == vchFromString("0"))
				{
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
									COffer &myLinkOffer = myLinkVtxPos.back();
									myLinkOffer.nQty += theEscrow.nQty;
									if(myLinkOffer.nQty < 0)
										myLinkOffer.nQty = 0;
									nQty = myLinkOffer.nQty;
									myLinkOffer.PutToOfferList(myLinkVtxPos);
									if (!dontaddtodb && !pofferdb->WriteOffer(dbOffer.vchLinkOffer, myLinkVtxPos))
									{
										errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4046 - " + _("Failed to write to offer link to DB");
										return error(errorMessage.c_str());
									}
									
								}
							}
							dbOffer.nQty = nQty;
							if(dbOffer.nQty < 0)
								dbOffer.nQty = 0;
							dbOffer.PutToOfferList(myVtxPos);
							if (!dontaddtodb && !pofferdb->WriteOffer(theEscrow.vchOffer, myVtxPos))
							{
								errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4047 - " + _("Failed to write to offer to DB");
								return error(errorMessage.c_str());
							}
						}			
					}
				}

				if(op == OP_ESCROW_COMPLETE)
				{
					if(vvchArgs[1] == vchFromString("1"))
					{
						int count = 0;
						// ensure we don't add same feedback twice (feedback in db should be older than current height)
						// ensure feedback is valid
						// ensure you aren't leaving feedback for yourself
						if(!serializedEscrow.buyerFeedback.IsNull()  && serializedEscrow.buyerFeedback.nFeedbackUser != BUYER && theEscrow.buyerFeedback.nHeight < nHeight)
						{
							theEscrow.buyerFeedback = serializedEscrow.buyerFeedback;
							theEscrow.buyerFeedback.nHeight = nHeight;
							theEscrow.buyerFeedback.txHash = tx.GetHash();
							count++;
						}
						else
							theEscrow.buyerFeedback.SetNull();

						if(!serializedEscrow.sellerFeedback.IsNull() && serializedEscrow.sellerFeedback.nFeedbackUser != SELLER && theEscrow.sellerFeedback.nHeight < nHeight)
						{
							theEscrow.sellerFeedback = serializedEscrow.sellerFeedback;
							theEscrow.sellerFeedback.nHeight = nHeight;
							theEscrow.sellerFeedback.txHash = tx.GetHash();
							count++;
						}
						else
							theEscrow.sellerFeedback.SetNull();


						if(!serializedEscrow.arbiterFeedback.IsNull() && serializedEscrow.arbiterFeedback.nFeedbackUser != ARBITER && theEscrow.arbiterFeedback.nHeight < nHeight)
						{
							theEscrow.arbiterFeedback = serializedEscrow.arbiterFeedback;
							theEscrow.arbiterFeedback.nHeight = nHeight;
							theEscrow.arbiterFeedback.txHash = tx.GetHash();
							count++;
						}
						else
							theEscrow.arbiterFeedback.SetNull();

						// can't leave more than 2 feedbacks at once
						if(count > 2)
						{
							errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4048 - " + _("Cannot leave more than 2 feedbacks in the same transaction");
							return true;
						}
						int numBuyerRatings, numSellerRatings, numArbiterRatings;
						int buyerFeedbackCount = FindFeedbackInEscrow(theEscrow.buyerFeedback.nFeedbackUser, BUYER, vtxPos, numBuyerRatings);
						int sellerFeedbackCount = FindFeedbackInEscrow(theEscrow.sellerFeedback.nFeedbackUser, SELLER, vtxPos, numSellerRatings);
						int arbiterFeedbackCount = FindFeedbackInEscrow(theEscrow.arbiterFeedback.nFeedbackUser, ARBITER, vtxPos, numArbiterRatings);
						// has this user (nFeedbackUser) already rated?
						if(numBuyerRatings > 0)
							theEscrow.buyerFeedback.nRating = 0;
						if(numSellerRatings > 0)
							theEscrow.sellerFeedback.nRating = 0;
						if(numArbiterRatings > 0)
							theEscrow.arbiterFeedback.nRating = 0;
						if(buyerFeedbackCount >= 10 && !serializedEscrow.buyerFeedback.IsNull())
						{
							errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4049 - " + _("Buyer cannot exceed 10 feedback entries for this escrow");
							return true;
						}
						if(sellerFeedbackCount >= 10 && !serializedEscrow.sellerFeedback.IsNull())
						{
							errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4050 - " + _("Seller cannot exceed 10 feedback entries for this escrow");
							return true;
						}
						if(arbiterFeedbackCount >= 10 && !serializedEscrow.arbiterFeedback.IsNull())
						{
							errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4051 - " + _("Arbiter cannot exceed 10 feedback entries for this escrow");
							return true;
						}
						if(!dontaddtodb)
							HandleEscrowFeedback(theEscrow);	
					
					}
					else
					{
						theOffer.UnserializeFromTx(tx);
						theEscrow.vchOfferAcceptLink = theOffer.accept.vchAcceptRand;
					}
				}
				
			}
			else
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4052 - " + _("Escrow not found when trying to update");
				return true;
			}
					
		}
		else
		{

			if (pescrowdb->ExistsEscrow(vvchArgs[0]))
			{
				errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4053 - " + _("Escrow already exists");
				return true;
			}
		
			vector<COffer> myVtxPos;
			// make sure offer is still valid and then deduct qty
			if (GetTxAndVtxOfOffer( theEscrow.vchOffer, dbOffer, txOffer, myVtxPos))
			{

				if(dbOffer.sCategory.size() > 0 && boost::algorithm::ends_with(stringFromVch(dbOffer.sCategory), "wanted"))
				{
					errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4054 - " + _("Cannot purchase a wanted offer");
				}
				else if(dbOffer.nQty != -1)
				{
					vector<COffer> myLinkVtxPos;
					unsigned int nQty = dbOffer.nQty - theEscrow.nQty;
					// if this is a linked offer we must update the linked offer qty aswell
					if (pofferdb->ExistsOffer(dbOffer.vchLinkOffer)) {
						if (pofferdb->ReadOffer(dbOffer.vchLinkOffer, myLinkVtxPos) && !myLinkVtxPos.empty())
						{
							COffer &myLinkOffer = myLinkVtxPos.back();
							myLinkOffer.nQty -= theEscrow.nQty;
							if(myLinkOffer.nQty < 0)
								myLinkOffer.nQty = 0;
							nQty = myLinkOffer.nQty;
							myLinkOffer.PutToOfferList(myLinkVtxPos);
							if (!dontaddtodb && !pofferdb->WriteOffer(dbOffer.vchLinkOffer, myLinkVtxPos))
							{
								errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4055 - " + _("Failed to write to offer link to DB");
								return true;
							}
							
						}
					}
					dbOffer.nQty = nQty;
					if(dbOffer.nQty < 0)
						dbOffer.nQty = 0;
					dbOffer.PutToOfferList(myVtxPos);
					if (!dontaddtodb && !pofferdb->WriteOffer(theEscrow.vchOffer, myVtxPos))
					{
						errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4056 - " + _("Failed to write to offer to DB");
						return true;
					}			
				}
			}
		}
		// if this is a null transaction its assumed to the the OP_ESCROW_COMPLETE null tx's sent by offeraccept, we need to set the op here because a null tx doesn't have data and thus cant set the op (op is needed for pruning)
		if(op == OP_ESCROW_COMPLETE)
			theEscrow.op = OP_ESCROW_COMPLETE;
        // set the escrow's txn-dependent values
		theEscrow.txHash = tx.GetHash();
		theEscrow.nHeight = nHeight;
		PutToEscrowList(vtxPos, theEscrow);
        // write escrow  
		
        if (!dontaddtodb && !pescrowdb->WriteEscrow(vvchArgs[0], vtxPos))
		{
			errorMessage = "SYSCOIN_ESCROW_CONSENSUS_ERROR: ERRCODE: 4057 - " + _("Failed to write to escrow DB");
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
void HandleEscrowFeedback(const CEscrow& escrow)
{
	if(escrow.buyerFeedback.nRating > 0)
	{
		CSyscoinAddress address = CSyscoinAddress(stringFromVch(escrow.vchBuyerAlias));
		if(address.IsValid() && address.isAlias)
		{
			vector<CAliasIndex> vtxPos;
			const vector<unsigned char> &vchAlias = vchFromString(address.aliasName);
			if (paliasdb->ReadAlias(vchAlias, vtxPos) && !vtxPos.empty())
			{
				
				CAliasIndex alias = vtxPos.back();
				alias.nRatingCount++;
				alias.nRating += escrow.buyerFeedback.nRating;
				PutToAliasList(vtxPos, alias);
				paliasdb->WriteAlias(vchAlias, vchFromString(address.ToString()), vtxPos);
			}
		}
			
	}
	if(escrow.sellerFeedback.nRating > 0)
	{
		CSyscoinAddress address = CSyscoinAddress(stringFromVch(escrow.vchSellerAlias));
		if(address.IsValid() && address.isAlias)
		{
			vector<CAliasIndex> vtxPos;
			const vector<unsigned char> &vchAlias = vchFromString(address.aliasName);
			if (paliasdb->ReadAlias(vchAlias, vtxPos) && !vtxPos.empty())
			{
				CAliasIndex alias = vtxPos.back();
				alias.nRatingCount++;
				alias.nRating += escrow.sellerFeedback.nRating;
				PutToAliasList(vtxPos, alias);
				paliasdb->WriteAlias(vchAlias, vchFromString(address.ToString()), vtxPos);
			}
		}
	}
	if(escrow.arbiterFeedback.nRating > 0)
	{
		CSyscoinAddress address = CSyscoinAddress(stringFromVch(escrow.vchArbiterAlias));
		if(address.IsValid() && address.isAlias)
		{
			vector<CAliasIndex> vtxPos;
			const vector<unsigned char> &vchAlias = vchFromString(address.aliasName);
			if (paliasdb->ReadAlias(vchAlias, vtxPos) && !vtxPos.empty())
			{
				CAliasIndex alias = vtxPos.back();
				alias.nRatingCount++;
				alias.nRating += escrow.arbiterFeedback.nRating;
				PutToAliasList(vtxPos, alias);
				paliasdb->WriteAlias(vchAlias, vchFromString(address.ToString()), vtxPos);
			}
		}
	}
}
int FindFeedbackInEscrow(const unsigned char nFeedbackUser, const EscrowUser type, const vector<CEscrow> &vtxPos, int &numRatings)
{
	int count = 0;
	numRatings = 0;
	for(unsigned int i =0;i<vtxPos.size();i++)
	{
		if(type == BUYER)
		{
			if(vtxPos[i].buyerFeedback.nFeedbackUser == nFeedbackUser)
			{
				count++;
				if(vtxPos[i].buyerFeedback.nRating > 0)
					numRatings++;
			}
		}
		else if(type == SELLER)
		{
			if(vtxPos[i].sellerFeedback.nFeedbackUser == nFeedbackUser)
			{
				count++;
				if(vtxPos[i].sellerFeedback.nRating > 0)
					numRatings++;
			}
		}
		else if(type == ARBITER)
		{
			if(vtxPos[i].arbiterFeedback.nFeedbackUser == nFeedbackUser)
			{
				count++;
				if(vtxPos[i].arbiterFeedback.nRating > 0)
					numRatings++;
			}
		}
	}
	return count;
}
void GetFeedbackInEscrow(vector<CEscrowFeedback> &feedBack, int &avgRating, const EscrowUser type, const vector<CEscrow> &vtxPos)
{
	float nRating = 0;
	int nRatingCount = 0;
	for(unsigned int i =0;i<vtxPos.size();i++)
	{
		if(type == BUYER)
		{
			if(!vtxPos[i].buyerFeedback.IsNull())
			{
				if(vtxPos[i].buyerFeedback.nRating > 0)
				{
					nRating += vtxPos[i].buyerFeedback.nRating;
					nRatingCount++;
				}
				feedBack.push_back(vtxPos[i].buyerFeedback);
			}
		}
		else if(type == SELLER)
		{
			if(!vtxPos[i].sellerFeedback.IsNull())
			{
				if(vtxPos[i].sellerFeedback.nRating > 0)
				{
					nRating += vtxPos[i].sellerFeedback.nRating;
					nRatingCount++;
				}
				feedBack.push_back(vtxPos[i].sellerFeedback);
			}
		}
		else if(type == ARBITER)
		{
			if(!vtxPos[i].arbiterFeedback.IsNull())
			{
				if(vtxPos[i].arbiterFeedback.nRating > 0)
				{
					nRating += vtxPos[i].arbiterFeedback.nRating;
					nRatingCount++;
				}
				feedBack.push_back(vtxPos[i].arbiterFeedback);
			}
		}
	}
	if(nRatingCount > 0)
	{
		nRating /= nRatingCount;
	}
	avgRating = (int)roundf(nRating);
	if(feedBack.size() > 0)
		sort(feedBack.begin(), feedBack.end(), escrowfeedbacksort());
	
}
UniValue escrownew(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 5 )
        throw runtime_error(
		"escrownew <alias> <offer> <quantity> <message> <arbiter alias>\n"
						"<alias> An alias you own.\n"
                        "<offer> GUID of offer that this escrow is managing.\n"
                        "<quantity> Quantity of items to buy of offer.\n"
						"<message> Delivery details to seller.\n"
						"<arbiter alias> Alias of Arbiter.\n"
                        + HelpRequiringPassphrase());
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	vector<unsigned char> vchOffer = vchFromValue(params[1]);
	string strArbiter = params[4].get_str();
	boost::algorithm::to_lower(strArbiter);
	// check for alias existence in DB
	CAliasIndex arbiteralias;
	CTransaction aliastx, buyeraliastx;
	if (!GetTxOfAlias(vchFromString(strArbiter), arbiteralias, aliastx))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4058 - " + _("Failed to read arbiter alias from DB"));
	

	vector<unsigned char> vchMessage = vchFromValue(params[3]);
	unsigned int nQty = 1;
	if(atof(params[2].get_str().c_str()) < 0)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4059 - " + _("Invalid quantity value, must be greator than 0"));

	try {
		nQty = boost::lexical_cast<unsigned int>(params[2].get_str());
	} catch (std::exception &e) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4060 - " + _("Invalid quantity value. Quantity must be less than 4294967296."));
	}

    if (vchMessage.size() <= 0)
        vchMessage = vchFromString("ESCROW");


	CAliasIndex buyeralias;
	if (!GetTxOfAlias(vchAlias, buyeralias, buyeraliastx))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4061 - " + _("Could not find buyer alias with this name"));

    if(!IsSyscoinTxMine(buyeraliastx, "alias")) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4062 - " + _("This alias is not yours."));
    }
	if (pwalletMain->GetWalletTx(buyeraliastx.GetHash()) == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4063 - " + _("This alias is not in your wallet"));

	COffer theOffer, linkedOffer;
	
	CTransaction txOffer, txAlias;
	vector<COffer> offerVtxPos;
	if (!GetTxAndVtxOfOffer( vchOffer, theOffer, txOffer, offerVtxPos, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4064 - " + _("Could not find an offer with this identifier"));

	CAliasIndex selleralias;
	if (!GetTxOfAlias( theOffer.vchAlias, selleralias, txAlias, true))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4065 - " + _("Could not find seller alias with this identifier"));

	unsigned int memPoolQty = QtyOfPendingAcceptsInMempool(vchOffer);
	if(theOffer.nQty != -1 && theOffer.nQty < (nQty+memPoolQty))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4066 - " + _("Not enough remaining quantity to fulfill this escrow"));

	if(theOffer.sCategory.size() > 0 && boost::algorithm::ends_with(stringFromVch(theOffer.sCategory), "wanted"))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4067 - " + _("Cannot purchase a wanted offer"));

	const CWalletTx *wtxAliasIn = NULL;

	CScript scriptPubKeyAlias, scriptPubKeyAliasOrig;
	COfferLinkWhitelistEntry foundEntry;
	if(!theOffer.vchLinkOffer.empty())
	{
	
		CTransaction tmpTx;
		vector<COffer> offerTmpVtxPos;
		if (!GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkedOffer, tmpTx, offerTmpVtxPos, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4068 - " + _("Trying to accept a linked offer but could not find parent offer"));
		CAliasIndex theLinkedAlias;
		CTransaction txLinkedAlias;
		if (!GetTxOfAlias( linkedOffer.vchAlias, theLinkedAlias, txLinkedAlias, true))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4069 - " + _("Could not find an alias with this identifier"));
		if (linkedOffer.bOnlyAcceptBTC)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4070 - " + _("Linked offer only accepts Bitcoins, linked offers currently only work with Syscoin payments"));
		selleralias = theLinkedAlias;
	}
	else
	{
		// if offer is not linked, look for a discount for the buyer
		theOffer.linkWhitelist.GetLinkEntryByHash(buyeralias.vchAlias, foundEntry);

		if(!foundEntry.IsNull())
		{
			// check for existing alias updates/transfers
			if (ExistsInMempool(buyeralias.vchAlias, OP_ALIAS_UPDATE)) {
				throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4071 - " + _("There is are pending operations on that alias"));
			}
			// make sure its in your wallet (you control this alias)
			if (IsSyscoinTxMine(buyeraliastx, "alias")) 
			{
				wtxAliasIn = pwalletMain->GetWalletTx(buyeraliastx.GetHash());		
				CPubKey currentKey(buyeralias.vchPubKey);
				scriptPubKeyAliasOrig = GetScriptForDestination(currentKey.GetID());
				scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << buyeralias.vchAlias  << buyeralias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
				scriptPubKeyAlias += scriptPubKeyAliasOrig;
			}			
		}
	}


	if (ExistsInMempool(vchOffer, OP_OFFER_ACTIVATE) || ExistsInMempool(vchOffer, OP_OFFER_UPDATE)) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4072 - " + _("There are pending operations on that offer"));
	}
	
    // gather inputs
	int64_t rand = GetRand(std::numeric_limits<int64_t>::max());
	vector<unsigned char> vchRand = CScriptNum(rand).getvch();
    vector<unsigned char> vchEscrow = vchFromValue(HexStr(vchRand));

    // this is a syscoin transaction
    CWalletTx wtx;
	EnsureWalletIsUnlocked();
    CScript scriptPubKey, scriptPubKeyBuyer, scriptPubKeySeller, scriptPubKeyArbiter,scriptBuyer, scriptSeller,scriptArbiter;

	string strCipherText = "";
	// encrypt to offer owner
	if(!EncryptMessage(selleralias.vchPubKey, vchMessage, strCipherText))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4073 - " + _("Could not encrypt message to seller"));
	
	if (strCipherText.size() > MAX_ENCRYPTED_VALUE_LENGTH)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4074 - " + _("Payment message length cannot exceed 1023 bytes"));

	CPubKey ArbiterPubKey(arbiteralias.vchPubKey);
	CPubKey SellerPubKey(selleralias.vchPubKey);
	CPubKey BuyerPubKey(buyeralias.vchPubKey);
	CSyscoinAddress selleraddy(SellerPubKey.GetID());
	CKeyID keyID;
	if (!selleraddy.GetKeyID(keyID))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4076 - " + _("Seller address does not refer to a key"));
	CKey vchSecret;
	if (pwalletMain->GetKey(keyID, vchSecret))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4077 - " + _("Cannot purchase your own offer"));
	
	scriptArbiter= GetScriptForDestination(ArbiterPubKey.GetID());
	scriptSeller= GetScriptForDestination(SellerPubKey.GetID());
	scriptBuyer= GetScriptForDestination(BuyerPubKey.GetID());
	UniValue arrayParams(UniValue::VARR);
	UniValue arrayOfKeys(UniValue::VARR);

	// standard 2 of 3 multisig
	arrayParams.push_back(2);
	arrayOfKeys.push_back(HexStr(arbiteralias.vchPubKey));
	arrayOfKeys.push_back(HexStr(selleralias.vchPubKey));
	arrayOfKeys.push_back(HexStr(buyeralias.vchPubKey));
	arrayParams.push_back(arrayOfKeys);
	UniValue resCreate;
	try
	{
		resCreate = tableRPC.execute("createmultisig", arrayParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}
	if (!resCreate.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4078 - " + _("Could not create escrow transaction: Invalid response from createescrow"));
	const UniValue &o = resCreate.get_obj();
	vector<unsigned char> redeemScript;
	const UniValue& redeemScript_value = find_value(o, "redeemScript");
	if (redeemScript_value.isStr())
	{
		redeemScript = ParseHex(redeemScript_value.get_str());
		scriptPubKey = CScript(redeemScript.begin(), redeemScript.end());
	}
	else
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4079 - " + _("Could not create escrow transaction: could not find redeem script in response"));
	// send to escrow address

	int precision = 2;
	CAmount nPricePerUnit = convertCurrencyCodeToSyscoin(theOffer.vchAliasPeg, theOffer.sCurrencyCode, theOffer.GetPrice(foundEntry), chainActive.Tip()->nHeight, precision);
	CAmount nTotal = nPricePerUnit*nQty;

	CAmount nEscrowFee = GetEscrowArbiterFee(nTotal);
	CRecipient recipientFee;
	CreateRecipient(scriptPubKey, recipientFee);
	CAmount nAmountWithFee = nTotal+nEscrowFee+recipientFee.nAmount;

	CWalletTx escrowWtx;
	vector<CRecipient> vecSendEscrow;
	CRecipient recipientEscrow  = {scriptPubKey, nAmountWithFee, false};
	vecSendEscrow.push_back(recipientEscrow);
	
	SendMoneySyscoin(vecSendEscrow, recipientEscrow.nAmount, false, escrowWtx, NULL, NULL, NULL, NULL, false);
	
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
	newEscrow.escrowInputTxHash = escrowWtx.GetHash();
	newEscrow.nPricePerUnit = nPricePerUnit;
	newEscrow.nHeight = chainActive.Tip()->nHeight;

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
	vector<CRecipient> vecSend;
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
	SendMoneySyscoin(vecSend,recipientBuyer.nAmount+ recipientArbiter.nAmount+recipientSeller.nAmount+aliasRecipient.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxInEscrow);
	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	res.push_back(HexStr(vchRand));
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
    if (!GetTxOfEscrow( vchEscrow, 
		escrow, tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4080 - " + _("Could not find a escrow with this key"));
    vector<vector<unsigned char> > vvch;
    int op, nOut;
    if (!DecodeEscrowTx(tx, op, nOut, vvch) 
    	|| !IsEscrowOp(op))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4081 - " + _("Could not decode escrow transaction"));
	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4082 - " + _("This escrow is not in your wallet"));

    // unserialize escrow UniValue from txn
    CEscrow theEscrow;
    if(!theEscrow.UnserializeFromTx(tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4083 - " + _("Cannot unserialize escrow from transaction"));
	vector<CEscrow> vtxPos;
	if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
		  throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4084 - " + _("Failed to read from escrow DB"));
    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, escrow.escrowInputTxHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4085 - " + _("Failed to find escrow transaction"));

	CAliasIndex arbiterAlias, buyerAlias, sellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction arbiteraliastx, buyeraliastx, selleraliastx;
	bool isExpired;
	CSyscoinAddress arbiterAddressOnActivate, arbiterAddress;
	CPubKey arbiterKey;
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
		arbiterAlias.nHeight = vtxPos.front().nHeight;
		arbiterAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(arbiterAlias.vchPubKey);
		arbiterAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}

	aliasVtxPos.clear();
	CSyscoinAddress buyerAddressOnActivate, buyerAddress;
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(sellerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
		buyerAlias.nHeight = vtxPos.front().nHeight;
		buyerAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(buyerAlias.vchPubKey);
		buyerAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}
	aliasVtxPos.clear();
	CSyscoinAddress sellerAddressOnActivate, sellerAddress;
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAddress = CSyscoinAddress(sellerKey.GetID());
		sellerAlias.nHeight = vtxPos.front().nHeight;
		sellerAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(sellerAlias.vchPubKey);
		sellerAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}


	bool foundSellerKey = false;
	try
	{
		// if this is the seller calling release, try to claim the release	
		CKeyID keyID;
		if (!sellerAddressOnActivate.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4086 - " + _("Seller address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4087 - " + _("Private key for seller address is not known"));
		foundSellerKey = true;
		
	}
	catch(...)
	{
		foundSellerKey = false;
	}
	if(foundSellerKey)
		return tableRPC.execute("escrowclaimrelease", params);
    if (op != OP_ESCROW_ACTIVATE)
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4088 - " + _("Release can only happen on an activated escrow"));
	int nOutMultiSig = 0;
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	CRecipient recipientFee;
	CreateRecipient(redeemScriptPubKey, recipientFee);
	int64_t nExpectedAmount = escrow.nPricePerUnit*escrow.nQty;
	int64_t nEscrowFee = GetEscrowArbiterFee(nExpectedAmount);
	int64_t nExpectedAmountWithFee = nExpectedAmount+nEscrowFee+recipientFee.nAmount;
	for(unsigned int i=0;i<fundingTx.vout.size();i++)
	{
		if(fundingTx.vout[i].nValue == nExpectedAmountWithFee)
		{
			nOutMultiSig = i;
			break;
		}
	} 
	int64_t nAmount = fundingTx.vout[nOutMultiSig].nValue;
	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());
	if(nAmount != nExpectedAmountWithFee)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4089 - " + _("Expected amount of escrow does not match what is held in escrow"));

	string strPrivateKey ;
	bool arbiterSigning = false;
	// who is initiating release arbiter or buyer?
	try
	{
		arbiterSigning = true;
		// try arbiter
		CKeyID keyID;
		if (!arbiterAddressOnActivate.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4090 - " + _("Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4091 - " + _("Private key for arbiter address is not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
	}
	catch(...)
	{
		arbiterSigning = false;
		// otherwise try buyer
		CKeyID keyID;
		if (!buyerAddressOnActivate.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4092 - " + _("Buyer or Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4093 - " + _("Buyer or Arbiter private keys not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
	}
    // check for existing escrow 's
	if (ExistsInMempool(vchEscrow, OP_ESCROW_ACTIVATE) || ExistsInMempool(vchEscrow, OP_ESCROW_RELEASE) || ExistsInMempool(vchEscrow, OP_ESCROW_REFUND) || ExistsInMempool(vchEscrow, OP_ESCROW_COMPLETE)) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4094 - " + _("There are pending operations on that escrow"));
	}
	// ensure that all is ok before trying to do the offer accept
	UniValue arrayAcceptParams(UniValue::VARR);
	arrayAcceptParams.push_back(stringFromVch(vchEscrow));
	arrayAcceptParams.push_back("1");
	try
	{
		tableRPC.execute("escrowcomplete", arrayAcceptParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}

	// create a raw tx that sends escrow amount to seller and collateral to buyer
    // inputs buyer txHash
	UniValue arrayCreateParams(UniValue::VARR);
	UniValue createTxInputsArray(UniValue::VARR);
	UniValue createTxInputUniValue(UniValue::VOBJ);
	UniValue createAddressUniValue(UniValue::VOBJ);
	createTxInputUniValue.push_back(Pair("txid", escrow.escrowInputTxHash.ToString()));
	createTxInputUniValue.push_back(Pair("vout", nOutMultiSig));
	createTxInputsArray.push_back(createTxInputUniValue);
	if(arbiterSigning)
	{
		createAddressUniValue.push_back(Pair(sellerAddress.ToString(), ValueFromAmount(nExpectedAmount)));
		createAddressUniValue.push_back(Pair(arbiterAddress.ToString(), ValueFromAmount(nEscrowFee)));
	}
	else
	{
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4095 - " + _("Could not create escrow transaction: Invalid response from createrawtransaction"));
	string createEscrowSpendingTx = resCreate.get_str();

	// Buyer/Arbiter signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);

	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", escrow.escrowInputTxHash.ToString()));
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4096 - " + _("Could not sign escrow transaction"));
	}	
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4097 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
	const UniValue& o = res.get_obj();
	string hex_str = "";

	const UniValue& hex_value = find_value(o, "hex");
	if (hex_value.isStr())
		hex_str = hex_value.get_str();


	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_RELEASE;
	escrow.rawTx = ParseHex(hex_str);
	escrow.nHeight = chainActive.Tip()->nHeight;
    CScript scriptPubKey, scriptPubKeySeller;
	scriptPubKeySeller= GetScriptForDestination(sellerKey.GetID());

	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));
    scriptPubKey << CScript::EncodeOP_N(OP_ESCROW_RELEASE) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKey += scriptPubKeySeller;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInAlias=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxInAlias, wtxIn);
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

    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
	vector<CEscrow> vtxPos;
    if (!GetTxAndVtxOfEscrow( vchEscrow, 
		escrow, tx, vtxPos))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4098 - " + _("Could not find a escrow with this key"));

	CAliasIndex sellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction aliastx;
	bool isExpired;
	CSyscoinAddress sellerAddressOnActivate;
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, aliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAlias.nHeight = vtxPos.front().nHeight;
		sellerAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(sellerAlias.vchPubKey);
		sellerAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}

    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, escrow.escrowInputTxHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4101 - " + _("Failed to find escrow transaction"));

	// make sure you can actually accept it before going through claim
	UniValue arrayAcceptParamsCheck(UniValue::VARR);
	arrayAcceptParamsCheck.push_back(stringFromVch(vchEscrow));
	arrayAcceptParamsCheck.push_back("1");
	try
	{
		tableRPC.execute("escrowcomplete", arrayAcceptParamsCheck);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}


 	int nOutMultiSig = 0;
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	CRecipient recipientFee;
	CreateRecipient(redeemScriptPubKey, recipientFee);
	int64_t nExpectedAmount = escrow.nPricePerUnit*escrow.nQty;
	int64_t nEscrowFee = GetEscrowArbiterFee(nExpectedAmount);
	int64_t nExpectedAmountWithFee = nExpectedAmount+nEscrowFee+recipientFee.nAmount;
	for(unsigned int i=0;i<fundingTx.vout.size();i++)
	{
		if(fundingTx.vout[i].nValue == nExpectedAmountWithFee)
		{
			nOutMultiSig = i;
			break;
		}
	} 
	int64_t nAmount = fundingTx.vout[nOutMultiSig].nValue;
	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());
	if(nAmount != nExpectedAmountWithFee)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4102 - " + _("Expected amount of escrow does not match what is held in escrow"));
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4103 - " + _("Could not decode escrow transaction: Invalid response from decoderawtransaction"));
	}


	CKeyID keyID;
	if (!sellerAddressOnActivate.GetKeyID(keyID))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4104 - " + _("Seller address does not refer to a key"));
	CKey vchSecret;
	if (!pwalletMain->GetKey(keyID, vchSecret))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4105 - " + _("Private key for seller address is not known"));
	const string &strPrivateKey = CSyscoinSecret(vchSecret).ToString();
	// check for existing escrow 's
	if (ExistsInMempool(vchEscrow, OP_ESCROW_ACTIVATE) || ExistsInMempool(vchEscrow, OP_ESCROW_RELEASE) || ExistsInMempool(vchEscrow, OP_ESCROW_REFUND) || ExistsInMempool(vchEscrow, OP_ESCROW_COMPLETE) ) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4106 - " + _("There are pending operations on that escrow"));
	}
    // Seller signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);
	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", escrow.escrowInputTxHash.ToString()));
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4107 - " + _("Could not sign escrow transaction"));
	}	
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4108 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4109 - " + _("Could not sign escrow transaction. It is showing as incomplete, you may not allowed to complete this request at this time"));

	// broadcast the payment transaction
	UniValue arraySendParams(UniValue::VARR);
	arraySendParams.push_back(hex_str);
	try
	{
		res = tableRPC.execute("sendrawtransaction", arraySendParams);
	}
	catch (UniValue& objError)
	{
		string rawtxError = find_value(objError, "message").get_str();
		UniValue arrayAcceptParams(UniValue::VARR);
		arrayAcceptParams.push_back(stringFromVch(vchEscrow));
		try
		{
			res = tableRPC.execute("escrowcomplete", arrayAcceptParams);
		}
		catch (UniValue& objError)
		{
			throw runtime_error(rawtxError);
		}
		if (!res.isArray())
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4110 - " + _("Could not complete escrow: Invalid response from escrowcomplete"));
		return res;
	}
	if (!res.isStr())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4111 - " + _("Could not send escrow transaction: Invalid response from sendrawtransaction"));


	UniValue arrayAcceptParams(UniValue::VARR);
	arrayAcceptParams.push_back(stringFromVch(vchEscrow));
	try
	{
		res = tableRPC.execute("escrowcomplete", arrayAcceptParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}
	if (!res.isArray())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4112 - " + _("Could not complete escrow: Invalid response from escrowcomplete"));
	return res;

	
	
}
UniValue escrowcomplete(const UniValue& params, bool fHelp) {
    if (fHelp || params.size()  < 1 || params.size() > 2)
        throw runtime_error(
		"escrowcomplete <escrow guid> [justcheck]\n"
                         "Accepts an offer that's in escrow, to complete the escrow process.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchEscrow = vchFromValue(params[0]);
	string justCheck = params.size()>=2?params[1].get_str():"0";

	EnsureWalletIsUnlocked();

    // look for a transaction with this key
	CWalletTx wtx;
	CTransaction tx;
	CEscrow escrow;
	if (!GetTxOfEscrow( vchEscrow, 
			escrow, tx))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4113 - " + _("Could not find a escrow with this key"));
	if(justCheck != "1")
	{
		uint256 hash;
		vector<vector<unsigned char> > vvch;
		int op, nOut;
		if (!DecodeEscrowTx(tx, op, nOut, vvch) 
    		|| !IsEscrowOp(op) 
    		|| (op != OP_ESCROW_RELEASE))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4114 - " + _("Can only complete an escrow that has been released to you"));
		const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
		if (wtxIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4115 - " + _("This escrow is not in your wallet"));
		
      		// check for existing escrow 's
		if (ExistsInMempool(vchEscrow, OP_ESCROW_ACTIVATE) || ExistsInMempool(vchEscrow, OP_ESCROW_RELEASE) || ExistsInMempool(vchEscrow, OP_ESCROW_REFUND) || ExistsInMempool(vchEscrow, OP_ESCROW_COMPLETE) ) {
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4116 - " + _("There are pending operations on that escrow"));
			}
	}
	UniValue acceptParams(UniValue::VARR);
	acceptParams.push_back(stringFromVch(escrow.vchBuyerAlias));
	acceptParams.push_back(stringFromVch(escrow.vchOffer));
	acceptParams.push_back(static_cast<ostringstream*>( &(ostringstream() << escrow.nQty) )->str());
	acceptParams.push_back(stringFromVch(escrow.vchPaymentMessage));
	acceptParams.push_back("");
	acceptParams.push_back("");
	acceptParams.push_back(tx.GetHash().GetHex());
	acceptParams.push_back(justCheck);

	UniValue res;
	try
	{
		res = tableRPC.execute("offeraccept", acceptParams);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}	
	if (!res.isArray())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4117 - " + _("Could not complete escrow transaction: Invalid response from offeraccept"));

	const UniValue &arr = res.get_array();
	uint256 acceptTxHash(uint256S(arr[0].get_str()));
	const string &acceptGUID = arr[1].get_str();
	const CWalletTx *wtxAcceptIn;
	wtxAcceptIn = pwalletMain->GetWalletTx(acceptTxHash);
	if(justCheck != "1")
	{
		if (wtxAcceptIn == NULL)
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4118 - " + _("Offer accept is not in your wallet"));
		UniValue ret(UniValue::VARR);
		ret.push_back(wtxAcceptIn->GetHash().GetHex());
		return ret;
	}
	else
	{
		UniValue ret(UniValue::VARR);
		ret.push_back("1");
		return ret;
	}
	
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
    if (!GetTxOfEscrow( vchEscrow, 
		escrow, tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4119 - " + _("Could not find a escrow with this key"));
    vector<vector<unsigned char> > vvch;
    int op, nOut;
    if (!DecodeEscrowTx(tx, op, nOut, vvch) 
    	|| !IsEscrowOp(op))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4120 - " + _("Could not decode escrow transaction"));
	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4121 - " + _("This escrow is not in your wallet"));
    // unserialize escrow from txn
    CEscrow theEscrow;
    if(!theEscrow.UnserializeFromTx(tx))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4122 - " + _("Cannot unserialize escrow from transaction"));
	vector<CEscrow> vtxPos;
	if (!pescrowdb->ReadEscrow(vchEscrow, vtxPos) || vtxPos.empty())
		  throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4123 - " + _("Failed to read from escrow DB"));
    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, escrow.escrowInputTxHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4124 - " + _("Failed to find escrow transaction"));

	CAliasIndex arbiterAlias, buyerAlias, sellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction arbiteraliastx, buyeraliastx, selleraliastx;
	bool isExpired;
	CSyscoinAddress arbiterAddressOnActivate, arbiterAddress;
	CPubKey arbiterKey;
	if(GetTxAndVtxOfAlias(escrow.vchArbiterAlias, arbiterAlias, arbiteraliastx, aliasVtxPos, isExpired, true))
	{
		arbiterKey = CPubKey(arbiterAlias.vchPubKey);
		arbiterAddress = CSyscoinAddress(arbiterKey.GetID());
		arbiterAlias.nHeight = vtxPos.front().nHeight;
		arbiterAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(arbiterAlias.vchPubKey);
		arbiterAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}

	aliasVtxPos.clear();
	CSyscoinAddress buyerAddressOnActivate, buyerAddress;
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, buyeraliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAddress = CSyscoinAddress(buyerKey.GetID());
		buyerAlias.nHeight = vtxPos.front().nHeight;
		buyerAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(buyerAlias.vchPubKey);
		buyerAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}
	aliasVtxPos.clear();
	CSyscoinAddress sellerAddressOnActivate;
	CPubKey sellerKey;
	if(GetTxAndVtxOfAlias(escrow.vchSellerAlias, sellerAlias, selleraliastx, aliasVtxPos, isExpired, true))
	{
		sellerKey = CPubKey(sellerAlias.vchPubKey);
		sellerAlias.nHeight = vtxPos.front().nHeight;
		sellerAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(sellerAlias.vchPubKey);
		sellerAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}
	bool foundBuyerKey = false;
	try
	{
		// if this is the buyer calling refund, try to claim the refund
		CKeyID keyID;
		if (!buyerAddressOnActivate.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4125 - " + _("Buyer address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4126 - " + _("Private key for buyer address is not known"));
		foundBuyerKey = true;
	}
	catch(...)
	{
		foundBuyerKey = false;
	}
	if(foundBuyerKey)
		return tableRPC.execute("escrowclaimrefund", params);
	if(op != OP_ESCROW_ACTIVATE)
		 throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4127 - " + _("Refund can only happen on an activated escrow"));
	int nOutMultiSig = 0;
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	CRecipient recipientFee;
	CreateRecipient(redeemScriptPubKey, recipientFee);
	int64_t nExpectedAmount = escrow.nPricePerUnit*escrow.nQty;
	int64_t nEscrowFee = GetEscrowArbiterFee(nExpectedAmount);
	int64_t nExpectedAmountWithFee = nExpectedAmount+nEscrowFee+recipientFee.nAmount;
	for(unsigned int i=0;i<fundingTx.vout.size();i++)
	{
		if(fundingTx.vout[i].nValue == nExpectedAmountWithFee)
		{
			nOutMultiSig = i;
			break;
		}
	} 
	int64_t nAmount = fundingTx.vout[nOutMultiSig].nValue;
	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());
	if(nAmount != nExpectedAmountWithFee)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4128 - " + _("Expected amount of escrow does not match what is held in escrow"));
	string strPrivateKey ;
	bool arbiterSigning = false;
	// who is initiating release arbiter or seller?
	try
	{
		arbiterSigning = true;
		// try arbiter
		CKeyID keyID;
		if (!arbiterAddressOnActivate.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4129 - " + _("Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4130 - " + _("Private key for arbiter address is not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
	}
	catch(...)
	{
		arbiterSigning = false;
		// otherwise try seller
		CKeyID keyID;
		if (!sellerAddressOnActivate.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4131 - " + _("Seller or Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4132 - " + _("Seller or Arbiter private keys not known"));
		strPrivateKey = CSyscoinSecret(vchSecret).ToString();
	}
     	// check for existing escrow 's
	if (ExistsInMempool(vchEscrow, OP_ESCROW_ACTIVATE) || ExistsInMempool(vchEscrow, OP_ESCROW_RELEASE) || ExistsInMempool(vchEscrow, OP_ESCROW_REFUND) || ExistsInMempool(vchEscrow, OP_ESCROW_COMPLETE) ) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4133 - " + _("There are pending operations on that escrow"));
	}
	// refunds buyer from escrow
	UniValue arrayCreateParams(UniValue::VARR);
	UniValue createTxInputsArray(UniValue::VARR);
	UniValue createTxInputUniValue(UniValue::VOBJ);
	UniValue createAddressUniValue(UniValue::VOBJ);
	createTxInputUniValue.push_back(Pair("txid", escrow.escrowInputTxHash.ToString()));
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4134 - " + _("Could not create escrow transaction: Invalid response from createrawtransaction"));
	string createEscrowSpendingTx = resCreate.get_str();

	// Buyer/Arbiter signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);

	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", escrow.escrowInputTxHash.ToString()));
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4135 - " + _("Could not sign escrow transaction"));
	}
	
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4136 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
	const UniValue& o = res.get_obj();
	string hex_str = "";

	const UniValue& hex_value = find_value(o, "hex");
	if (hex_value.isStr())
		hex_str = hex_value.get_str();

	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_REFUND;
	escrow.rawTx = ParseHex(hex_str);
	escrow.nHeight = chainActive.Tip()->nHeight;

    CScript scriptPubKey, scriptPubKeyBuyer;
	scriptPubKeyBuyer= GetScriptForDestination(buyerKey.GetID());
	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));
    scriptPubKey << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("0") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKey += scriptPubKeyBuyer;
	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInAlias=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxInAlias, wtxIn);
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

    // look for a transaction with this key
    CTransaction tx;
	CEscrow escrow;
	vector<CEscrow> vtxPos;
    if (!GetTxAndVtxOfEscrow( vchEscrow, 
		escrow, tx, vtxPos))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4137 - " + _("Could not find a escrow with this key"));

	CAliasIndex arbiterAlias, buyerAlias, sellerAlias;
	vector<CAliasIndex> aliasVtxPos;
	CTransaction aliastx;
	bool isExpired;
	GetTxOfAlias(escrow.vchArbiterAlias, arbiterAlias, aliastx, true);
	CPubKey arbiterKey(arbiterAlias.vchPubKey);

	CSyscoinAddress buyerAddressOnActivate;
	CPubKey buyerKey;
	if(GetTxAndVtxOfAlias(escrow.vchBuyerAlias, buyerAlias, aliastx, aliasVtxPos, isExpired, true))
	{
		buyerKey = CPubKey(buyerAlias.vchPubKey);
		buyerAlias.nHeight = vtxPos.front().nHeight;
		buyerAlias.GetAliasFromList(aliasVtxPos);
		CPubKey pubKey(buyerAlias.vchPubKey);
		buyerAddressOnActivate = CSyscoinAddress(pubKey.GetID());
	}

	GetTxOfAlias(escrow.vchSellerAlias, sellerAlias, aliastx, true);
	CPubKey sellerKey(sellerAlias.vchPubKey);
	CSyscoinAddress sellerAddress(sellerKey.GetID());

	CWalletTx wtx;
	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4138 - " + _("This escrow is not in your wallet"));

    CTransaction fundingTx;
	if (!GetSyscoinTransaction(vtxPos.front().nHeight, escrow.escrowInputTxHash, fundingTx, Params().GetConsensus()))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4140 - " + _("Failed to find escrow transaction"));

 	int nOutMultiSig = 0;
	// 0.5% escrow fee
	CScript redeemScriptPubKey = CScript(escrow.vchRedeemScript.begin(), escrow.vchRedeemScript.end());
	CRecipient recipientFee;
	CreateRecipient(redeemScriptPubKey, recipientFee);
	int64_t nExpectedAmount = escrow.nPricePerUnit*escrow.nQty;
	int64_t nEscrowFee = GetEscrowArbiterFee(nExpectedAmount);
	int64_t nExpectedAmountWithFee = nExpectedAmount+nEscrowFee+recipientFee.nAmount;
	for(unsigned int i=0;i<fundingTx.vout.size();i++)
	{
		if(fundingTx.vout[i].nValue == nExpectedAmountWithFee)
		{
			nOutMultiSig = i;
			break;
		}
	} 
	int64_t nAmount = fundingTx.vout[nOutMultiSig].nValue;
	string strEscrowScriptPubKey = HexStr(fundingTx.vout[nOutMultiSig].scriptPubKey.begin(), fundingTx.vout[nOutMultiSig].scriptPubKey.end());
	if(nAmount != nExpectedAmountWithFee)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4141 - " + _("Expected amount of escrow does not match what is held in escrow"));
	// decode rawTx and check it pays enough and it pays to buyer appropriately
	// check that right amount is going to be sent to buyer
	bool foundBuyerPayment = false;
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4142 - " + _("Could not decode escrow transaction: Invalid response from decoderawtransaction"));
	

	// get buyer's private key for signing
	CKeyID keyID;
	if (!buyerAddressOnActivate.GetKeyID(keyID))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4144 - " + _("Buyer address does not refer to a key"));
	CKey vchSecret;
	if (!pwalletMain->GetKey(keyID, vchSecret))
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4145 - " + _("Private key for buyer address is not known"));
	string strPrivateKey = CSyscoinSecret(vchSecret).ToString();
      	// check for existing escrow 's
	if (ExistsInMempool(vchEscrow, OP_ESCROW_ACTIVATE) || ExistsInMempool(vchEscrow, OP_ESCROW_RELEASE) || ExistsInMempool(vchEscrow, OP_ESCROW_REFUND) || ExistsInMempool(vchEscrow, OP_ESCROW_COMPLETE)  ) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4146 - " + _("There are pending operations on that escrow"));
	}
    // buyer signs it
	UniValue arraySignParams(UniValue::VARR);
	UniValue arraySignInputs(UniValue::VARR);
	UniValue arrayPrivateKeys(UniValue::VARR);
	UniValue signUniValue(UniValue::VOBJ);
	signUniValue.push_back(Pair("txid", escrow.escrowInputTxHash.ToString()));
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4147 - " + _("Could not sign escrow transaction"));
	}
	if (!res.isObject())
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4148 - " + _("Could not sign escrow transaction: Invalid response from signrawtransaction"));
	
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4149 - " + _("Could not sign escrow transaction. It is showing as incomplete, you may not allowed to complete this request at this time"));

	// broadcast the payment transaction
	UniValue arraySendParams(UniValue::VARR);
	arraySendParams.push_back(hex_str);
	UniValue ret(UniValue::VARR);
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
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4150 - " + _("Could not send escrow transaction: Invalid response from sendrawtransaction"));

	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_COMPLETE;
	escrow.nHeight = chainActive.Tip()->nHeight;
    CScript scriptPubKeyBuyer, scriptPubKeySeller,scriptPubKeyArbiter, scriptPubKeyBuyerDestination, scriptPubKeySellerDestination, scriptPubKeyArbiterDestination;
	scriptPubKeyBuyerDestination= GetScriptForDestination(buyerKey.GetID());

	const vector<unsigned char> &data = escrow.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashEscrow = vchFromValue(HexStr(vchHash));
    scriptPubKeyBuyer << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyBuyer += scriptPubKeyBuyerDestination;
 
	scriptPubKeySellerDestination= GetScriptForDestination(sellerKey.GetID());
    scriptPubKeySeller << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeySeller += scriptPubKeySellerDestination;

	scriptPubKeyArbiterDestination= GetScriptForDestination(arbiterKey.GetID());
    scriptPubKeyArbiter << CScript::EncodeOP_N(OP_ESCROW_REFUND) << vchEscrow << vchFromString("1") << vchHashEscrow << OP_2DROP << OP_2DROP;
    scriptPubKeyArbiter += scriptPubKeyArbiterDestination;

	vector<CRecipient> vecSend;
	CRecipient recipientBuyer;
	CreateRecipient(scriptPubKeyBuyer, recipientBuyer);
	vecSend.push_back(recipientBuyer);
	CRecipient recipientSeller;
	CreateRecipient(scriptPubKeySeller, recipientSeller);
	vecSend.push_back(recipientSeller);
	CRecipient recipientArbiter;
	CreateRecipient(scriptPubKeyArbiter, recipientArbiter);
	vecSend.push_back(recipientArbiter);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInAlias=NULL;
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+recipientSeller.nAmount+recipientArbiter.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxInAlias, wtxIn);
	ret.push_back(wtx.GetHash().GetHex());
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
			nRatingPrimary = atoi(params[2].get_str());

		} catch (std::exception &e) {
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4151 - " + _("Invalid primary rating value"));
		}
	}
	if(params.size() > 3)
		vchFeedbackSecondary = vchFromValue(params[3]);
	if(params.size() > 4)
	{
		try {
			nRatingSecondary = atoi(params[4].get_str());

		} catch (std::exception &e) {
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4152 - " + _("Invalid secondary rating value"));
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
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4153 - " + _("Could not find a escrow with this key"));
    vector<vector<unsigned char> > vvch;
    int op, nOut;
    if (!DecodeEscrowTx(tx, op, nOut, vvch) 
    	|| !IsEscrowOp(op))
        throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4154 - " + _("Could not decode escrow transaction"));
	const CWalletTx *wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4155 - " + _("This escrow is not in your wallet"));
	CAliasIndex arbiterAlias, buyerAlias, sellerAlias;
	CTransaction aliastx;
	GetTxOfAlias(escrow.vchArbiterAlias, arbiterAlias, aliastx, true);
	CPubKey arbiterKey(arbiterAlias.vchPubKey);
	CSyscoinAddress arbiterAddress(arbiterKey.GetID());

	GetTxOfAlias(escrow.vchBuyerAlias, buyerAlias, aliastx, true);
	CPubKey buyerKey(buyerAlias.vchPubKey);
	CSyscoinAddress buyerAddress(buyerKey.GetID());

	GetTxOfAlias(escrow.vchSellerAlias, sellerAlias, aliastx, true);
	CPubKey sellerKey(sellerAlias.vchPubKey);
	CSyscoinAddress sellerAddress(sellerKey.GetID());
	bool foundBuyerKey = false;
	try
	{
		CKeyID keyID;
		if (!buyerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4156 - " + _("Buyer address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4157 - " + _("Private key for buyer address is not known"));
		foundBuyerKey = true;
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
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4158 - " + _("Seller address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4159 - " + _("Private key for seller address is not known"));
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
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4160 - " + _("Arbiter address does not refer to a key"));
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4161 - " + _("Private key for arbiter address is not known"));
		foundArbiterKey = true;
	}
	catch(...)
	{
		foundArbiterKey = false;
	}
     	// check for existing escrow 's
	if (ExistsInMempool(vchEscrow, OP_ESCROW_ACTIVATE) || ExistsInMempool(vchEscrow, OP_ESCROW_RELEASE) || ExistsInMempool(vchEscrow, OP_ESCROW_REFUND) || ExistsInMempool(vchEscrow, OP_ESCROW_COMPLETE) ) {
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4162 - " + _("There are pending operations on that escrow"));
	}
	escrow.ClearEscrow();
	escrow.op = OP_ESCROW_COMPLETE;
	escrow.nHeight = chainActive.Tip()->nHeight;
	// buyer
	if(foundBuyerKey)
	{
		CEscrowFeedback sellerFeedback(BUYER);
		sellerFeedback.vchFeedback = vchFeedbackPrimary;
		sellerFeedback.nRating = nRatingPrimary;
		sellerFeedback.nHeight = chainActive.Tip()->nHeight;
		CEscrowFeedback arbiterFeedback(BUYER);
		arbiterFeedback.vchFeedback = vchFeedbackSecondary;
		arbiterFeedback.nRating = nRatingSecondary;
		arbiterFeedback.nHeight = chainActive.Tip()->nHeight;
		escrow.arbiterFeedback = arbiterFeedback;
		escrow.sellerFeedback = sellerFeedback;
	}
	// seller
	else if(foundSellerKey)
	{
		CEscrowFeedback buyerFeedback(SELLER);
		buyerFeedback.vchFeedback = vchFeedbackPrimary;
		buyerFeedback.nRating = nRatingPrimary;
		buyerFeedback.nHeight = chainActive.Tip()->nHeight;
		CEscrowFeedback arbiterFeedback(SELLER);
		arbiterFeedback.vchFeedback = vchFeedbackSecondary;
		arbiterFeedback.nRating = nRatingSecondary;
		arbiterFeedback.nHeight = chainActive.Tip()->nHeight;
		escrow.buyerFeedback = buyerFeedback;
		escrow.arbiterFeedback = arbiterFeedback;
	}
	// arbiter
	else if(foundArbiterKey)
	{
		CEscrowFeedback buyerFeedback(ARBITER);
		buyerFeedback.vchFeedback = vchFeedbackPrimary;
		buyerFeedback.nRating = nRatingPrimary;
		buyerFeedback.nHeight = chainActive.Tip()->nHeight;
		CEscrowFeedback sellerFeedback(ARBITER);
		sellerFeedback.vchFeedback = vchFeedbackSecondary;
		sellerFeedback.nRating = nRatingSecondary;
		sellerFeedback.nHeight = chainActive.Tip()->nHeight;
		escrow.buyerFeedback = buyerFeedback;
		escrow.sellerFeedback = sellerFeedback;
	}
	else
	{
		throw runtime_error("SYSCOIN_ESCROW_RPC_ERROR: ERRCODE: 4163 - " + _("You must be either the arbiter, buyer or seller to leave feedback on this escrow"));
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

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInAlias=NULL;
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+recipientSeller.nAmount+recipientArbiter.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxInAlias, wtxIn);
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

    string sHeight = strprintf("%llu", ca.nHeight);
    oEscrow.push_back(Pair("escrow", stringFromVch(vchEscrow)));
	string sTime;
	CBlockIndex *pindex = chainActive[ca.nHeight];
	if (pindex) {
		sTime = strprintf("%llu", pindex->nTime);
	}
	int avgBuyerRating, avgSellerRating, avgArbiterRating;
	vector<CEscrowFeedback> buyerFeedBacks, sellerFeedBacks, arbiterFeedBacks;
	GetFeedbackInEscrow(buyerFeedBacks, avgBuyerRating, BUYER, vtxPos);
	GetFeedbackInEscrow(sellerFeedBacks, avgSellerRating, SELLER, vtxPos);
	GetFeedbackInEscrow(arbiterFeedBacks, avgArbiterRating, ARBITER, vtxPos);
		
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
	oEscrow.push_back(Pair("offeracceptlink", stringFromVch(ca.vchOfferAcceptLink)));
	oEscrow.push_back(Pair("quantity", strprintf("%d", ca.nQty)));
	oEscrow.push_back(Pair("systotal", ValueFromAmount(ca.nPricePerUnit * ca.nQty)));
	int64_t nEscrowFee = GetEscrowArbiterFee(ca.nPricePerUnit * ca.nQty);
	oEscrow.push_back(Pair("sysfee", ValueFromAmount(nEscrowFee)));
	string sTotal = strprintf("%" PRIu64" SYS", ((nEscrowFee+ca.nPricePerUnit)*ca.nQty)/COIN);
	oEscrow.push_back(Pair("total", sTotal));
    oEscrow.push_back(Pair("txid", ca.txHash.GetHex()));
    oEscrow.push_back(Pair("height", sHeight));
	string strMessage = string("");
	if(!DecryptMessage(theSellerAlias.vchPubKey, ca.vchPaymentMessage, strMessage))
		strMessage = string("Encrypted for owner of offer");
	oEscrow.push_back(Pair("pay_message", strMessage));
	oEscrow.push_back(Pair("rawpay_message", stringFromVch(ca.vchPaymentMessage)));
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
		oFeedback.push_back(Pair("feedbackuser", buyerFeedBacks[i].nFeedbackUser));
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
		oFeedback.push_back(Pair("feedbackuser", sellerFeedBacks[i].nFeedbackUser));
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
		CBlockIndex *pindex = chainActive[buyerFeedBacks[i].nHeight];
		if (pindex) {
			sFeedbackTime = strprintf("%llu", pindex->nTime);
		}
		oFeedback.push_back(Pair("txid", arbiterFeedBacks[i].txHash.GetHex()));
		oFeedback.push_back(Pair("time", sFeedbackTime));
		oFeedback.push_back(Pair("rating", arbiterFeedBacks[i].nRating));
		oFeedback.push_back(Pair("feedbackuser", arbiterFeedBacks[i].nFeedbackUser));
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
    uint64_t nHeight;
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
		}
		COffer offer;
		CTransaction offertx;
		vector<COffer> offerVtxPos;
		GetTxAndVtxOfOffer(escrow.vchOffer, offer, offertx, offerVtxPos, true);

		// skip this escrow if it doesn't match the given filter value
		if (vchNameUniq.size() > 0 && vchNameUniq != vchEscrow)
			continue;
		// get last active name only
		if (vNamesI.find(vchEscrow) != vNamesI.end() && (escrow.nHeight <= vNamesI[vchEscrow] || vNamesI[vchEscrow] < 0))
			continue;

		nHeight = escrow.nHeight;
        // build the output
        UniValue oName(UniValue::VOBJ);
        oName.push_back(Pair("escrow", stringFromVch(vchEscrow)));
		string sTime;
		CBlockIndex *pindex = chainActive[escrow.nHeight];
		if (pindex) {
			sTime = strprintf("%llu", pindex->nTime);
		}
		int avgBuyerRating, avgSellerRating, avgArbiterRating;
		vector<CEscrowFeedback> buyerFeedBacks, sellerFeedBacks, arbiterFeedBacks;
		GetFeedbackInEscrow(buyerFeedBacks, avgBuyerRating, BUYER, vtxPos);
		GetFeedbackInEscrow(sellerFeedBacks, avgSellerRating, SELLER, vtxPos);
		GetFeedbackInEscrow(arbiterFeedBacks, avgArbiterRating, ARBITER, vtxPos);
		


		oName.push_back(Pair("time", sTime));
		oName.push_back(Pair("seller", stringFromVch(escrow.vchSellerAlias)));
		oName.push_back(Pair("arbiter", stringFromVch(escrow.vchArbiterAlias)));
		oName.push_back(Pair("buyer", stringFromVch(escrow.vchBuyerAlias)));
		oName.push_back(Pair("offer", stringFromVch(escrow.vchOffer)));
		oName.push_back(Pair("offertitle", stringFromVch(offer.sTitle)));
		oName.push_back(Pair("offeracceptlink", stringFromVch(escrow.vchOfferAcceptLink)));
		int64_t nEscrowFee = GetEscrowArbiterFee(escrow.nPricePerUnit * escrow.nQty);
		oName.push_back(Pair("sysfee", ValueFromAmount(nEscrowFee)));
		string sTotal = strprintf("%" PRIu64" SYS", ((nEscrowFee+escrow.nPricePerUnit)*escrow.nQty)/COIN);
		oName.push_back(Pair("total", sTotal));

		expired_block = nHeight + GetEscrowExpirationDepth();
        if(expired_block < chainActive.Tip()->nHeight && escrow.op == OP_ESCROW_COMPLETE)
		{
			expired = 1;
		} 
	
		string status = "unknown";
		if(pending == 0)
		{
			if(op == OP_ESCROW_ACTIVATE)
				status = "in escrow";
			else if(op == OP_ESCROW_RELEASE)
				status = "escrow released";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("0"))
				status = "escrow refunded";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("1"))
				status = "escrow refund complete";
			else if(op == OP_ESCROW_COMPLETE )
				status = "complete";
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
			oFeedback.push_back(Pair("feedbackuser", buyerFeedBacks[i].nFeedbackUser));
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
			oFeedback.push_back(Pair("feedbackuser", sellerFeedBacks[i].nFeedbackUser));
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
			CBlockIndex *pindex = chainActive[buyerFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("txid", arbiterFeedBacks[i].txHash.GetHex()));
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", arbiterFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", arbiterFeedBacks[i].nFeedbackUser));
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
		oName.push_back(Pair("status", status));
		oName.push_back(Pair("expired", expired));
 
		vNamesI[vchEscrow] = nHeight;
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
				
            // decode txn, skip non-alias txns
            vector<vector<unsigned char> > vvch;
            int op, nOut;
            if (!DecodeEscrowTx(tx, op, nOut, vvch) 
            	|| !IsEscrowOp(op) )
                continue;
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
			oEscrow.push_back(Pair("offeracceptlink", stringFromVch(txPos2.vchOfferAcceptLink)));

			int64_t nEscrowFee = GetEscrowArbiterFee(txPos2.nPricePerUnit * txPos2.nQty);
			oEscrow.push_back(Pair("sysfee", ValueFromAmount(nEscrowFee)));
			string sTotal = strprintf("%" PRIu64" SYS", ((nEscrowFee+txPos2.nPricePerUnit)*txPos2.nQty)/COIN);
			oEscrow.push_back(Pair("total", sTotal));
			if(nHeight + GetEscrowExpirationDepth() - chainActive.Tip()->nHeight <= 0  && txPos2.op == OP_ESCROW_COMPLETE)
			{
				expired = 1;
			}  
	
			string status = "unknown";

			if(op == OP_ESCROW_ACTIVATE)
				status = "in escrow";
			else if(op == OP_ESCROW_RELEASE)
				status = "escrow released";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("0"))
				status = "escrow refunded";
			else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("1"))
				status = "escrow refund complete";
			else if(op == OP_ESCROW_COMPLETE)
				status = "complete";

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
		vector<COffer> vtxOfferPos;
		vector<CEscrow> vtxPos;
		COffer offer;
		if (pofferdb->ReadOffer(txEscrow.vchOffer, vtxOfferPos) && !vtxOfferPos.empty())
		{
			offer = vtxOfferPos.back();
		}
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
		vector<CEscrowFeedback> buyerFeedBacks, sellerFeedBacks, arbiterFeedBacks;
		if (!pescrowdb->ReadEscrow(pairScan.first, vtxPos) || vtxPos.empty())
			continue;
		GetFeedbackInEscrow(buyerFeedBacks, avgBuyerRating, BUYER, vtxPos);
		GetFeedbackInEscrow(sellerFeedBacks, avgSellerRating, SELLER, vtxPos);
		GetFeedbackInEscrow(arbiterFeedBacks, avgArbiterRating, ARBITER, vtxPos);
		
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
		oEscrow.push_back(Pair("offeracceptlink", stringFromVch(txEscrow.vchOfferAcceptLink)));
	
		string status = "unknown";

		if(op == OP_ESCROW_ACTIVATE)
			status = "in escrow";
		else if(op == OP_ESCROW_RELEASE)
			status = "escrow released";
		else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("0"))
			status = "escrow refunded";
		else if(op == OP_ESCROW_REFUND && vvch[1] == vchFromString("1"))
			status = "escrow refund complete";
		else if(op == OP_ESCROW_COMPLETE)
			status = "complete";
		

		oEscrow.push_back(Pair("status", status));
		int64_t nEscrowFee = GetEscrowArbiterFee(txEscrow.nPricePerUnit * txEscrow.nQty);
		oEscrow.push_back(Pair("sysfee", ValueFromAmount(nEscrowFee)));
		string sTotal = strprintf("%" PRIu64" SYS", ((nEscrowFee+txEscrow.nPricePerUnit)*txEscrow.nQty)/COIN);
		oEscrow.push_back(Pair("total", sTotal));
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
			oFeedback.push_back(Pair("feedbackuser", buyerFeedBacks[i].nFeedbackUser));
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
			oFeedback.push_back(Pair("feedbackuser", sellerFeedBacks[i].nFeedbackUser));
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
			CBlockIndex *pindex = chainActive[buyerFeedBacks[i].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", arbiterFeedBacks[i].nRating));
			oFeedback.push_back(Pair("feedbackuser", arbiterFeedBacks[i].nFeedbackUser));
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

        oRes.push_back(oEscrow);
    }


	return oRes;
}