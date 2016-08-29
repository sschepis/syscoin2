#ifndef FEEDBACK_H
#define FEEDBACK_H
#include "script/script.h"
#include "serialize.h"
enum FeedbackUser {
    FEEDBACKBUYER=1,
	FEEDBACKSELLER=2,
	FEEDBACKARBITER=3
};
class CFeedback {
public:
	std::vector<unsigned char> vchFeedback;
	unsigned char nRating;
	unsigned char nFeedbackUser;
	uint64_t nHeight;
	uint256 txHash;
    CFeedback() {
        SetNull();
    }
    CFeedback(unsigned char nAcceptFeedbackUser) {
        SetNull();
		nFeedbackUser = nAcceptFeedbackUser;
    }
    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
		READWRITE(vchFeedback);
		READWRITE(nRating);
		READWRITE(nFeedbackUser);
		READWRITE(nHeight);
		READWRITE(txHash);
	}

    friend bool operator==(const CFeedback &a, const CFeedback &b) {
        return (
        a.vchFeedback == b.vchFeedback
		&& a.nRating == b.nRating
		&& a.nFeedbackUser == b.nFeedbackUser
		&& a.nHeight == b.nHeight
		&& a.txHash == b.txHash
        );
    }

    CFeedback operator=(const CFeedback &b) {
        vchFeedback = b.vchFeedback;
		nRating = b.nRating;
		nFeedbackUser = b.nFeedbackUser;
		nHeight = b.nHeight;
		txHash = b.txHash;
        return *this;
    }

    friend bool operator!=(const CFeedback &a, const CFeedback &b) {
        return !(a == b);
    }

    void SetNull() { txHash.SetNull(); nHeight = 0; nRating = 0; nFeedbackUser = 0; vchFeedback.clear();}
    bool IsNull() const { return ( txHash.IsNull() && nHeight == 0 && nRating == 0 && nFeedbackUser == 0 && vchFeedback.empty()); }
};
struct feedbacksort {
    bool operator ()(const CFeedback& a, const CFeedback& b) {
        return a.nHeight < b.nHeight;
    }
};
#endif // FEEDBACK_H