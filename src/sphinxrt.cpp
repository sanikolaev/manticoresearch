//
// Copyright (c) 2017-2018, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxint.h"
#include "sphinxrt.h"
#include "sphinxsearch.h"
#include "sphinxutils.h"
#include "sphinxjson.h"
#include "sphinxplugin.h"
#include "sphinxrlp.h"
#include "sphinxqcache.h"

#include <sys/stat.h>
#include <fcntl.h>

#if USE_WINDOWS
#include <io.h> // for open(), close()
#include <errno.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

//////////////////////////////////////////////////////////////////////////

#define BINLOG_WRITE_BUFFER		256*1024
#define BINLOG_AUTO_FLUSH		1000000

#define RTDICT_CHECKPOINT_V3			1024
#define RTDICT_CHECKPOINT_V5			48
#define SPH_RT_DOUBLE_BUFFER_PERCENT	10

#define WORDID_MAX				U64C(0xffffffffffffffff)

//////////////////////////////////////////////////////////////////////////

#ifndef NDEBUG
#define Verify(_expr) assert(_expr)
#else
#define Verify(_expr) _expr
#endif

//////////////////////////////////////////////////////////////////////////
// GLOBALS
//////////////////////////////////////////////////////////////////////////

/// publicly exposed binlog interface
ISphBinlog *			g_pBinlog				= NULL;

/// actual binlog implementation
class RtBinlog_c;
static RtBinlog_c *		g_pRtBinlog				= NULL;

/// protection from concurrent changes during binlog replay
static bool				g_bRTChangesAllowed		= false;

// optimize mode for disk chunks merge
static bool g_bProgressiveMerge = true;
static auto& g_bShutdown = sphGetShutdown();

//////////////////////////////////////////////////////////////////////////

// !COMMIT cleanup extern ref to sphinx.cpp
extern void sphSortDocinfos ( DWORD * pBuf, int iCount, int iStride );

// !COMMIT yes i am when debugging
#ifndef NDEBUG
#define PARANOID 1
#endif

//////////////////////////////////////////////////////////////////////////

// Variable Length Byte (VLB) encoding
// store int variable in as much bytes as actually needed to represent it
template < typename T, typename P >
static inline void ZipT ( CSphVector < BYTE, P > * pOut, T uValue )
{
	do
	{
		BYTE bOut = (BYTE)( uValue & 0x7f );
		uValue >>= 7;
		if ( uValue )
			bOut |= 0x80;
		pOut->Add ( bOut );
	} while ( uValue );
}

template < typename T >
static inline void ZipT ( BYTE * & pOut, T uValue )
{
	do
	{
		BYTE bOut = (BYTE)( uValue & 0x7f );
		uValue >>= 7;
		if ( uValue )
			bOut |= 0x80;
		*pOut++ = bOut;
	} while ( uValue );
}

#define SPH_MAX_KEYWORD_LEN (3*SPH_MAX_WORD_LEN+4)
STATIC_ASSERT ( SPH_MAX_KEYWORD_LEN<255, MAX_KEYWORD_LEN_SHOULD_FITS_BYTE );


// Variable Length Byte (VLB) decoding
template < typename T >
static inline const BYTE * UnzipT ( T * pValue, const BYTE * pIn )
{
	T uValue = 0;
	BYTE bIn;
	int iOff = 0;

	do
	{
		bIn = *pIn++;
		uValue += ( T ( bIn & 0x7f ) ) << iOff;
		iOff += 7;
	} while ( bIn & 0x80 );

	*pValue = uValue;
	return pIn;
}

#define ZipDword ZipT<DWORD>
#define ZipQword ZipT<uint64_t>
#define UnzipDword UnzipT<DWORD>
#define UnzipQword UnzipT<uint64_t>

#define ZipDocid ZipQword
#define ZipWordid ZipQword
#define UnzipDocid UnzipQword
#define UnzipWordid UnzipQword

//////////////////////////////////////////////////////////////////////////

struct CmpHitPlain_fn
{
	inline bool IsLess ( const CSphWordHit & a, const CSphWordHit & b ) const
	{
		return 	( a.m_uWordID<b.m_uWordID ) ||
			( a.m_uWordID==b.m_uWordID && a.m_uDocID<b.m_uDocID ) ||
			( a.m_uWordID==b.m_uWordID && a.m_uDocID==b.m_uDocID && a.m_uWordPos<b.m_uWordPos );
	}
};


struct CmpHitKeywords_fn
{
	const BYTE * m_pBase;
	explicit CmpHitKeywords_fn ( const BYTE * pBase ) : m_pBase ( pBase ) {}
	inline bool IsLess ( const CSphWordHit & a, const CSphWordHit & b ) const
	{
		const BYTE * pPackedA = m_pBase + a.m_uWordID;
		const BYTE * pPackedB = m_pBase + b.m_uWordID;
		int iCmp = sphDictCmpStrictly ( (const char *)pPackedA+1, *pPackedA, (const char *)pPackedB+1, *pPackedB );
		return 	( iCmp<0 ) ||
			( iCmp==0 && a.m_uDocID<b.m_uDocID ) ||
			( iCmp==0 && a.m_uDocID==b.m_uDocID && a.m_uWordPos<b.m_uWordPos );
	}
};


template < typename DOCID = SphDocID_t >
struct RtDoc_T
{
	DOCID						m_uDocID = 0;	///< my document id
	DWORD						m_uDocFields = 0;	///< fields mask
	DWORD						m_uHits = 0;	///< hit count
	DWORD						m_uHit = 0;		///< either index into segment hits, or the only hit itself (if hit count is 1)
};

template < typename WORDID=SphWordID_t >
struct RtWord_T
{
	union
	{
		WORDID					m_uWordID;	///< my keyword id
		const BYTE *			m_sWord;
		typename WIDEST<WORDID, const BYTE *>::T m_null = 0;
	};
	DWORD						m_uDocs = 0;	///< document count (for stats and/or BM25)
	DWORD						m_uHits = 0;	///< hit count (for stats and/or BM25)
	DWORD						m_uDoc = 0;		///< index into segment docs
};

typedef RtDoc_T<> RtDoc_t;
typedef RtWord_T<> RtWord_t;

struct RtWordCheckpoint_t
{
	union
	{
		SphWordID_t					m_uWordID;
		const char *				m_sWord;
	};
	int							m_iOffset;
};

// More than just sorted vector.
// OrderedHash is for fast (without sorting potentially big vector) inserts.
class CSphKilllist : public ISphNoncopyable
{
private:
	static const int				MAX_SMALL_SIZE = 512;
	CSphVector<SphDocID_t>			m_dLargeKlist;
	CSphOrderedHash < bool, SphDocID_t, IdentityHash_fn, MAX_SMALL_SIZE >	m_hSmallKlist;
	CSphRwlock						m_tLock;

public:

	CSphKilllist()
	{
		m_tLock.Init();
	}

	virtual ~CSphKilllist()
	{
		m_tLock.Done();
	}

	void Flush ( CSphVector<SphDocID_t> & dKlist ) REQUIRES (!m_tLock)
	{
		{
			CSphScopedRLock tRguard ( m_tLock );
			bool bGotHash = (m_hSmallKlist.GetLength ()>0);
			if ( !bGotHash )
				NakedCopy ( dKlist );

			if ( !bGotHash )
				return;
		}

		CSphScopedWLock tWguard ( m_tLock );
		NakedFlush ( nullptr, 0 );
		NakedCopy ( dKlist );
	}

	inline void Add ( SphDocID_t * pDocs, int iCount ) REQUIRES (!m_tLock)
	{
		if ( !iCount )
			return;

		CSphScopedWLock tWlock ( m_tLock );
		if ( m_hSmallKlist.GetLength()+iCount>=MAX_SMALL_SIZE )
		{
			NakedFlush ( pDocs, iCount );
		} else
		{
			while ( iCount-- )
				m_hSmallKlist.Add ( true, *pDocs++ );
		}
	}

	bool Exists ( SphDocID_t uDoc ) REQUIRES (!m_tLock)
	{
		CSphScopedRLock tRguard ( m_tLock );
		bool bGot = ( m_hSmallKlist.Exists ( uDoc ) || m_dLargeKlist.BinarySearch ( uDoc )!=NULL );
		return bGot;
	}

	void Reset ( SphDocID_t * pDocs, int iCount ) REQUIRES ( !m_tLock )
	{
		m_tLock.WriteLock();
		m_dLargeKlist.Reset();
		m_hSmallKlist.Reset();

		NakedFlush ( pDocs, iCount );

		m_tLock.Unlock();
	}

	void LoadFromFile ( const char * sFilename ) REQUIRES ( !m_tLock );
	void SaveToFile ( const char * sFilename ) REQUIRES ( !m_tLock );

private:

	void NakedCopy ( CSphVector<SphDocID_t> & dKlist )
	{
		assert ( m_hSmallKlist.GetLength()==0 );
		if ( !m_dLargeKlist.GetLength() )
			return;

		dKlist.Append ( m_dLargeKlist );
	}

	void NakedFlush ( SphDocID_t * pDocs, int iCount )
	{
		if ( m_hSmallKlist.GetLength()==0 && iCount==0 )
			return;

		m_dLargeKlist.Reserve ( m_dLargeKlist.GetLength()+m_hSmallKlist.GetLength()+iCount );
		m_hSmallKlist.IterateStart();
		while ( m_hSmallKlist.IterateNext() )
			m_dLargeKlist.Add ( m_hSmallKlist.IterateGetKey() );
		if ( pDocs && iCount )
			m_dLargeKlist.Append ( pDocs, iCount );
		m_dLargeKlist.Uniq();
		m_hSmallKlist.Reset();
	}
};

// is already id32<>id64 safe
void CSphKilllist::LoadFromFile ( const char * sFilename )
{
	Reset ( NULL, 0 );

	CSphString sName, sError;
	sName.SetSprintf ( "%s.kill", sFilename );
	if ( !sphIsReadable ( sName.cstr(), &sError ) )
		return;

	CSphAutoreader tKlistReader;
	if ( !tKlistReader.Open ( sName, sError ) )
		return;

	// FIXME!!! got rid of locks here
	m_tLock.WriteLock();
	m_dLargeKlist.Resize ( tKlistReader.GetDword() );
	SphDocID_t uLastDocID = 0;
	ARRAY_FOREACH ( i, m_dLargeKlist )
	{
		uLastDocID += ( SphDocID_t ) tKlistReader.UnzipOffset();
		m_dLargeKlist[i] = uLastDocID;
	};
	m_tLock.Unlock();
}

void CSphKilllist::SaveToFile ( const char * sFilename )
{
	// FIXME!!! got rid of locks here
	m_tLock.WriteLock();
	NakedFlush ( nullptr, 0 );

	CSphWriter tKlistWriter;
	CSphString sName, sError;
	sName.SetSprintf ( "%s.kill", sFilename );
	tKlistWriter.OpenFile ( sName.cstr(), sError );

	tKlistWriter.PutDword ( m_dLargeKlist.GetLength() );
	SphDocID_t uLastDocID = 0;
	ARRAY_FOREACH ( i, m_dLargeKlist )
	{
		tKlistWriter.ZipOffset ( m_dLargeKlist[i] - uLastDocID );
		uLastDocID = ( SphDocID_t ) m_dLargeKlist[i];
	};
	m_tLock.Unlock();
	tKlistWriter.CloseFile();
}


struct KlistRefcounted_t : ISphRefcountedMT
{
	CSphFixedVector<SphDocID_t>		m_dKilled { 0 };
private:
	~KlistRefcounted_t() = default;
};


// this is what actually stores index data
// RAM chunk consists of such segments
struct RtSegment_t : ISphNoncopyable
{
protected:
	static const int			KLIST_ACCUM_THRESH	= 32;

public:
	static CSphAtomic			m_iSegments;	///< age tag sequence generator
	int							m_iTag;			///< segment age tag

	CSphTightVector<BYTE>			m_dWords;
	CSphVector<RtWordCheckpoint_t>	m_dWordCheckpoints;
	CSphTightVector<uint64_t>		m_dInfixFilterCP;
	CSphTightVector<BYTE>		m_dDocs;
	CSphTightVector<BYTE>		m_dHits;

	int							m_iRows = 0;		///< number of actually allocated rows
	int							m_iAliveRows = 0;	///< number of alive (non-killed) rows
	CSphTightVector<CSphRowitem>		m_dRows;		///< row data storage
	KlistRefcounted_t *			m_pKlist;
	bool						m_bTlsKlist = false;	///< whether to apply TLS K-list during merge (must only be used by writer during Commit())
	CSphTightVector<BYTE>		m_dStrings;		///< strings storage
	CSphTightVector<DWORD>		m_dMvas;		///< MVAs storage
	CSphVector<BYTE>			m_dKeywordCheckpoints;
	mutable CSphAtomic			m_tRefCount;

	RtSegment_t ()
	{
		m_iTag = m_iSegments.Inc();
		m_dStrings.Add ( 0 ); // dummy zero offset
		m_dMvas.Add ( 0 ); // dummy zero offset
		m_pKlist = new KlistRefcounted_t();
	}

	~RtSegment_t ()
	{
		SafeRelease ( m_pKlist );
	}


	int64_t GetUsedRam () const
	{
		// FIXME! gonna break on vectors over 2GB
		return
			(int64_t)m_dWords.AllocatedBytes() +
			(int64_t)m_dDocs.AllocatedBytes() +
			(int64_t)m_dHits.AllocatedBytes() +
			(int64_t)m_dStrings.AllocatedBytes() +
			(int64_t)m_dMvas.AllocatedBytes() +
			(int64_t)m_dKeywordCheckpoints.AllocatedBytes() +
			(int64_t)m_dRows.AllocatedBytes() +
			(int64_t)m_dInfixFilterCP.AllocatedBytes();
	}

	int GetMergeFactor () const
	{
		return m_iRows;
	}

	int GetStride () const
	{
		return ( m_dRows.GetLength() / m_iRows );
	}

	const CSphFixedVector<SphDocID_t> & GetKlist() const { return m_pKlist->m_dKilled; }

	const CSphRowitem *		FindRow ( SphDocID_t uDocid ) const;
	const CSphRowitem *		FindAliveRow ( SphDocID_t uDocid ) const;
};

CSphAtomic RtSegment_t::m_iSegments { 0 };
const CSphRowitem * RtSegment_t::FindRow ( SphDocID_t uDocid ) const
{
	// binary search through the rows
	int iStride = GetStride();
	SphDocID_t uL = DOCINFO2ID ( m_dRows.Begin() );
	SphDocID_t uR = DOCINFO2ID ( &m_dRows[m_dRows.GetLength()-iStride] );

	if ( uDocid==uL )
		return m_dRows.Begin();

	if ( uDocid==uR )
		return &m_dRows[m_dRows.GetLength()-iStride];

	if ( uDocid<uL || uDocid>uR )
		return NULL;

	int iL = 0;
	int iR = m_iRows-1;
	while ( iR-iL>1 )
	{
		int iM = iL + (iR-iL)/2;
		SphDocID_t uM = DOCINFO2ID ( &m_dRows[iM*iStride] );

		if ( uDocid==uM )
			return &m_dRows[iM*iStride];
		else if ( uDocid>uM )
			iL = iM;
		else
			iR = iM;
	}
	return NULL;
}


const CSphRowitem * RtSegment_t::FindAliveRow ( SphDocID_t uDocid ) const
{
	if ( m_pKlist->m_dKilled.BinarySearch ( uDocid ) )
		return NULL;
	return FindRow ( uDocid );
}

//////////////////////////////////////////////////////////////////////////

struct RtDocWriter_t
{
	CSphTightVector<BYTE> *		m_pDocs;
	SphDocID_t					m_uLastDocID = 0;

	explicit RtDocWriter_t ( RtSegment_t * pSeg )
		: m_pDocs ( &pSeg->m_dDocs )
	{}

	void ZipDoc ( const RtDoc_t & tDoc )
	{
		CSphTightVector<BYTE> * pDocs = m_pDocs;
		BYTE * pEnd = pDocs->AddN ( 12*sizeof(DWORD) );
		const BYTE * pBegin = pDocs->Begin();

		ZipDocid ( pEnd, tDoc.m_uDocID - m_uLastDocID );
		m_uLastDocID = tDoc.m_uDocID;
		ZipDword ( pEnd, tDoc.m_uDocFields );
		ZipDword ( pEnd, tDoc.m_uHits );
		if ( tDoc.m_uHits==1 )
		{
			ZipDword ( pEnd, tDoc.m_uHit & 0xffffffUL );
			ZipDword ( pEnd, tDoc.m_uHit>>24 );
		} else
			ZipDword ( pEnd, tDoc.m_uHit );

		pDocs->Resize ( pEnd-pBegin );
	}

	DWORD ZipDocPtr () const
	{
		return m_pDocs->GetLength();
	}

	void ZipRestart ()
	{
		m_uLastDocID = 0;
	}
};

template < typename DOCID = SphDocID_t >
struct RtDocReader_T
{
	typedef RtDoc_T<DOCID>	RTDOC;
	const BYTE *	m_pDocs;
	int				m_iLeft;
	RTDOC			m_tDoc;

	template < typename RTWORD >
	RtDocReader_T ( const RtSegment_t * pSeg, const RTWORD & tWord )
	{
		m_pDocs = ( pSeg->m_dDocs.Begin() ? pSeg->m_dDocs.Begin() + tWord.m_uDoc : NULL );
		m_iLeft = tWord.m_uDocs;
		m_tDoc.m_uDocID = 0;
	}

	RtDocReader_T ()
	{
		m_pDocs = NULL;
		m_iLeft = 0;
		m_tDoc.m_uDocID = 0;
	}

	const RTDOC * UnzipDoc ()
	{
		if ( !m_iLeft || !m_pDocs )
			return NULL;

		const BYTE * pIn = m_pDocs;
		SphDocID_t uDeltaID;
		pIn = UnzipDocid ( &uDeltaID, pIn );
		m_tDoc.m_uDocID += (DOCID) uDeltaID;
		DWORD uField;
		pIn = UnzipDword ( &uField, pIn );
		m_tDoc.m_uDocFields = uField;
		pIn = UnzipDword ( &m_tDoc.m_uHits, pIn );
		if ( m_tDoc.m_uHits==1 )
		{
			DWORD a, b;
			pIn = UnzipDword ( &a, pIn );
			pIn = UnzipDword ( &b, pIn );
			m_tDoc.m_uHit = a + ( b<<24 );
		} else
			pIn = UnzipDword ( &m_tDoc.m_uHit, pIn );
		m_pDocs = pIn;

		m_iLeft--;
		return &m_tDoc;
	}
};

typedef RtDocReader_T<> RtDocReader_t;

template < typename VECTOR >
static int sphPutBytes ( VECTOR * pOut, const void * pData, int iLen )
{
	int iOff = pOut->GetLength();
	pOut->Resize ( iOff + iLen );
	memcpy ( pOut->Begin()+iOff, pData, iLen );
	return iOff;
}


struct RtWordWriter_t
{
	CSphTightVector<BYTE> *				m_pWords;
	CSphVector<RtWordCheckpoint_t> *	m_pCheckpoints;
	CSphVector<BYTE> *					m_pKeywordCheckpoints;

	CSphKeywordDeltaWriter				m_tLastKeyword;
	SphWordID_t							m_uLastWordID;
	DWORD								m_uLastDoc;
	int									m_iWords;

	bool								m_bKeywordDict;
	int									m_iWordsCheckpoint;

	RtWordWriter_t ( RtSegment_t * pSeg, bool bKeywordDict, int iWordsCheckpoint )
		: m_pWords ( &pSeg->m_dWords )
		, m_pCheckpoints ( &pSeg->m_dWordCheckpoints )
		, m_pKeywordCheckpoints ( &pSeg->m_dKeywordCheckpoints )
		, m_uLastWordID ( 0 )
		, m_uLastDoc ( 0 )
		, m_iWords ( 0 )
		, m_bKeywordDict ( bKeywordDict )
		, m_iWordsCheckpoint ( iWordsCheckpoint )
	{
		assert ( !m_pWords->GetLength() );
		assert ( !m_pCheckpoints->GetLength() );
		assert ( !m_pKeywordCheckpoints->GetLength() );
	}

	void ZipWord ( const RtWord_t & tWord )
	{
		CSphTightVector<BYTE> * pWords = m_pWords;
		if ( ++m_iWords==m_iWordsCheckpoint )
		{
			RtWordCheckpoint_t & tCheckpoint = m_pCheckpoints->Add();
			if ( !m_bKeywordDict )
			{
				tCheckpoint.m_uWordID = tWord.m_uWordID;
			} else
			{
				int iLen = tWord.m_sWord[0];
				assert ( iLen && iLen-1<SPH_MAX_KEYWORD_LEN );
				tCheckpoint.m_uWordID = sphPutBytes ( m_pKeywordCheckpoints, tWord.m_sWord+1, iLen+1 );
				m_pKeywordCheckpoints->Last() = '\0'; // checkpoint is NULL terminating string

				// reset keywords delta encoding
				m_tLastKeyword.Reset();
			}
			tCheckpoint.m_iOffset = pWords->GetLength();

			m_uLastWordID = 0;
			m_uLastDoc = 0;
			m_iWords = 1;
		}

		if ( !m_bKeywordDict )
		{
			ZipWordid ( pWords, tWord.m_uWordID - m_uLastWordID );
		} else
		{
			m_tLastKeyword.PutDelta ( *this, tWord.m_sWord+1, tWord.m_sWord[0] );
		}

		BYTE * pEnd = pWords->AddN ( 4*sizeof(DWORD) );
		const BYTE * pBegin = pWords->Begin();

		ZipDword ( pEnd, tWord.m_uDocs );
		ZipDword ( pEnd, tWord.m_uHits );
		ZipDword ( pEnd, tWord.m_uDoc - m_uLastDoc );

		pWords->Resize ( pEnd-pBegin );

		m_uLastWordID = tWord.m_uWordID;
		m_uLastDoc = tWord.m_uDoc;
	}

	void PutBytes ( const BYTE * pData, int iLen )
	{
		sphPutBytes ( m_pWords, pData, iLen );
	}
};

template < typename WORDID = SphWordID_t >
struct RtWordReader_T
{
	using RTWORD = RtWord_T<WORDID>;
	BYTE			m_tPackedWord[SPH_MAX_KEYWORD_LEN+1];
	const BYTE *	m_pCur = nullptr;
	const BYTE *	m_pMax = nullptr;
	RTWORD			m_tWord;
	int				m_iWords = 0;

	bool			m_bWordDict;
	int				m_iWordsCheckpoint;
	int				m_iCheckpoint = 0;

	RtWordReader_T ( const RtSegment_t * pSeg, bool bWordDict, int iWordsCheckpoint )
		: m_bWordDict ( bWordDict )
		, m_iWordsCheckpoint ( iWordsCheckpoint )
	{
		m_tWord.m_uWordID = 0;
		Reset ( pSeg );
		if ( bWordDict )
			m_tWord.m_sWord = m_tPackedWord;
	}

	void Reset ( const RtSegment_t * pSeg )
	{
		m_pCur = pSeg->m_dWords.Begin();
		m_pMax = m_pCur + pSeg->m_dWords.GetLength();

		m_tWord.m_uDoc = 0;
		m_iWords = 0;
	}

	const RTWORD * UnzipWord ()
	{
		if ( ++m_iWords==m_iWordsCheckpoint )
		{
			m_tWord.m_uDoc = 0;
			m_iWords = 1;
			++m_iCheckpoint;
			if ( !m_bWordDict )
				m_tWord.m_uWordID = 0;
		}
		if ( m_pCur>=m_pMax )
			return nullptr;

		const BYTE * pIn = m_pCur;
		DWORD uDeltaDoc;
		if ( m_bWordDict )
		{
			BYTE iMatch, iDelta, uPacked;
			uPacked = *pIn++;
			if ( uPacked & 0x80 )
			{
				iDelta = ( ( uPacked>>4 ) & 7 ) + 1;
				iMatch = uPacked & 15;
			} else
			{
				iDelta = uPacked & 127;
				iMatch = *pIn++;
			}
			m_tPackedWord[0] = iMatch+iDelta;
			memcpy ( m_tPackedWord+1+iMatch, pIn, iDelta );
			m_tPackedWord[1+m_tPackedWord[0]] = 0;
			pIn += iDelta;
		} else
		{
			SphWordID_t uDeltaID;
			pIn = UnzipWordid ( &uDeltaID, pIn );
			m_tWord.m_uWordID += (WORDID) uDeltaID;
		}
		pIn = UnzipDword ( &m_tWord.m_uDocs, pIn );
		pIn = UnzipDword ( &m_tWord.m_uHits, pIn );
		pIn = UnzipDword ( &uDeltaDoc, pIn );
		m_pCur = pIn;

		m_tWord.m_uDoc += uDeltaDoc;
		return &m_tWord;
	}
};

typedef RtWordReader_T<SphWordID_t> RtWordReader_t;

struct RtHitWriter_t
{
	CSphTightVector<BYTE> *		m_pHits;
	DWORD						m_uLastHit = 0;

	explicit RtHitWriter_t ( RtSegment_t * pSeg )
		: m_pHits ( &pSeg->m_dHits )
	{}

	void ZipHit ( DWORD uValue )
	{
		ZipDword ( m_pHits, uValue - m_uLastHit );
		m_uLastHit = uValue;
	}

	void ZipRestart ()
	{
		m_uLastHit = 0;
	}

	DWORD ZipHitPtr () const
	{
		return m_pHits->GetLength();
	}
};


struct RtHitReader_t
{
	const BYTE *	m_pCur;
	DWORD			m_iLeft;
	DWORD			m_uLast;

	RtHitReader_t ()
		: m_pCur ( NULL )
		, m_iLeft ( 0 )
		, m_uLast ( 0 )
	{}

	template < typename RTDOC >
	explicit RtHitReader_t ( const RtSegment_t * pSeg, const RTDOC * pDoc )
	{
		m_pCur = pSeg->m_dHits.Begin() + pDoc->m_uHit;
		m_iLeft = pDoc->m_uHits;
		m_uLast = 0;
	}

	DWORD UnzipHit ()
	{
		if ( !m_iLeft )
			return 0;

		DWORD uValue;
		m_pCur = UnzipDword ( &uValue, m_pCur );
		m_uLast += uValue;
		m_iLeft--;
		return m_uLast;
	}
};


struct RtHitReader2_t : public RtHitReader_t
{
	const BYTE * m_pBase;

	RtHitReader2_t ()
		: m_pBase ( NULL )
	{}

	void Seek ( SphOffset_t uOff, int iHits )
	{
		m_pCur = m_pBase + uOff;
		m_iLeft = iHits;
		m_uLast = 0;
	}
};

//////////////////////////////////////////////////////////////////////////

/// forward ref
struct RtIndex_t;

/// indexing accumulator
class RtAccum_t : public ISphRtAccum
{
public:
	int							m_iAccumDocs = 0;
	CSphTightVector<CSphWordHit>	m_dAccum;
	CSphTightVector<CSphRowitem>	m_dAccumRows;
	CSphVector<SphDocID_t>		m_dAccumKlist;
	CSphTightVector<BYTE>		m_dStrings;
	CSphTightVector<DWORD>		m_dMvas;
	CSphVector<DWORD>			m_dPerDocHitsCount;

	bool						m_bKeywordDict;
	CSphDictRefPtr_c			m_pDict;
	CSphDict *					m_pRefDict = nullptr; // not owned, used only for ==-matching

private:
	ISphRtDictWraperRefPtr_c	m_pDictRt;
	bool						m_bReplace = false;	///< insert or replace mode (affects CleanupDuplicates() behavior)
	void				ResetDict ();
public:
					explicit RtAccum_t ( bool bKeywordDict );
	void			SetupDict ( const ISphRtIndex * pIndex, CSphDict * pDict, bool bKeywordDict );
	void			Sort ();

	enum EWhatClear { EPartial=1, EAccum=2, ERest=4, EAll=7};
	void Cleanup ( BYTE eWhat=EAll );

	void			AddDocument ( ISphHits * pHits, const CSphMatch & tDoc, bool bReplace, int iRowSize, const char ** ppStr, const CSphVector<DWORD> & dMvas );
	RtSegment_t *	CreateSegment ( int iRowSize, int iWordsCheckpoint );
	void			CleanupDuplicates ( int iRowSize );
	void			GrabLastWarning ( CSphString & sWarning );
	void			SetIndex ( ISphRtIndex * pIndex ) { m_pIndex = pIndex; }
};

/// TLS indexing accumulator (we disallow two uncommitted adds within one thread; and so need at most one)
static SphThreadKey_t g_tTlsAccumKey;

/// binlog file view of the index
/// everything that a given log file needs to know about an index
struct BinlogIndexInfo_t
{
	CSphString	m_sName;				///< index name
	int64_t		m_iMinTID = INT64_MAX;	///< min TID logged by this file
	int64_t		m_iMaxTID = 0;			///< max TID logged by this file
	int64_t		m_iFlushedTID = 0;		///< last flushed TID
	int64_t		m_tmMin = INT64_MAX;	///< min TID timestamp
	int64_t		m_tmMax = 0;			///< max TID timestamp

	CSphIndex *	m_pIndex = nullptr;		///< replay only; associated index (might be NULL if we don't serve it anymore!)
	RtIndex_t *	m_pRT = nullptr;		///< replay only; RT index handle (might be NULL if N/A or non-RT)
	int64_t		m_iPreReplayTID = 0;	///< replay only; index TID at the beginning of this file replay
};

/// binlog file descriptor
/// file id (aka extension), plus a list of associated index infos
struct BinlogFileDesc_t
{
	int								m_iExt = 0;
	CSphVector<BinlogIndexInfo_t>	m_dIndexInfos;
};

/// Bin Log Operation
enum Blop_e
{
	BLOP_COMMIT			= 1,
	BLOP_UPDATE_ATTRS	= 2,
	BLOP_ADD_INDEX		= 3,
	BLOP_ADD_CACHE		= 4,
	BLOP_RECONFIGURE	= 5,

	BLOP_TOTAL
};

// forward declaration
class BufferReader_t;
class RtBinlog_c;


class BinlogWriter_c : public CSphWriter
{
public:
					BinlogWriter_c ();
	virtual			~BinlogWriter_c () {}

	virtual	void	Flush ();
	void			Write ();
	void			Fsync ();
	bool			HasUnwrittenData () const { return m_iPoolUsed>0; }
	bool			HasUnsyncedData () const { return m_iLastFsyncPos!=m_iLastWritePos; }

	void			ResetCrc ();	///< restart checksumming
	void			WriteCrc ();	///< finalize and write current checksum to output stream


private:
	int64_t			m_iLastWritePos;
	int64_t			m_iLastFsyncPos;
	int				m_iLastCrcPos;

	DWORD			m_uCRC;
	void			HashCollected ();
};


class BinlogReader_c : public CSphAutoreader
{
public:
					BinlogReader_c ();

	void			ResetCrc ();
	bool			CheckCrc ( const char * sOp, const char * sIndexName, int64_t iTid, int64_t iTxnPos );

private:
	DWORD			m_uCRC;
	int				m_iLastCrcPos;
	virtual void	UpdateCache ();
	void			HashCollected ();
};


class RtBinlog_c : public ISphBinlog
{
public:
	RtBinlog_c ();
	~RtBinlog_c ();

	void	BinlogCommit ( int64_t * pTID, const char * sIndexName, const RtSegment_t * pSeg, const CSphVector<SphDocID_t> & dKlist, bool bKeywordDict );
	void	BinlogUpdateAttributes ( int64_t * pTID, const char * sIndexName, const CSphAttrUpdate & tUpd );
	void	BinlogReconfigure ( int64_t * pTID, const char * sIndexName, const CSphReconfigureSetup & tSetup );
	void	NotifyIndexFlush ( const char * sIndexName, int64_t iTID, bool bShutdown );

	void	Configure ( const CSphConfigSection & hSearchd, bool bTestMode );
	void	Replay ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, ProgressCallbackSimple_t * pfnProgressCallback );

	void	GetFlushInfo ( BinlogFlushInfo_t & tFlush );
	bool	IsActive ()			{ return !m_bDisabled; }
	void	CheckPath ( const CSphConfigSection & hSearchd, bool bTestMode );

private:
	static const DWORD		BINLOG_VERSION = 6;

	static const DWORD		BINLOG_HEADER_MAGIC = 0x4c425053;	/// magic 'SPBL' header that marks binlog file
	static const DWORD		BLOP_MAGIC = 0x214e5854;			/// magic 'TXN!' header that marks binlog entry
	static const DWORD		BINLOG_META_MAGIC = 0x494c5053;		/// magic 'SPLI' header that marks binlog meta

	int64_t					m_iFlushTimeLeft;
	volatile int			m_iFlushPeriod;

	enum OnCommitAction_e
	{
		ACTION_NONE,
		ACTION_FSYNC,
		ACTION_WRITE
	};
	OnCommitAction_e		m_eOnCommit;

	CSphMutex				m_tWriteLock; // lock on operation

	int						m_iLockFD;
	CSphString				m_sWriterError;
	BinlogWriter_c			m_tWriter;

	mutable CSphVector<BinlogFileDesc_t>	m_dLogFiles; // active log files

	CSphString				m_sLogPath;

	bool					m_bReplayMode; // replay mode indicator
	bool					m_bDisabled;

	int						m_iRestartSize; // binlog size restart threshold

	// replay stats
	mutable int				m_iReplayedRows=0;

private:
	static void				DoAutoFlush ( void * pBinlog );
	int 					GetWriteIndexID ( const char * sName, int64_t iTID, int64_t tmNow );
	void					LoadMeta ();
	void					SaveMeta ();
	void					LockFile ( bool bLock );
	void					DoCacheWrite ();
	void					CheckDoRestart ();
	void					CheckDoFlush ();
	void					OpenNewLog ( int iLastState=0 );

	int						ReplayBinlog ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, int iBinlog );
	bool					ReplayCommit ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const;
	bool					ReplayUpdateAttributes ( int iBinlog, BinlogReader_c & tReader ) const;
	bool					ReplayIndexAdd ( int iBinlog, const SmallStringHash_T<CSphIndex*> & hIndexes, BinlogReader_c & tReader ) const;
	bool					ReplayCacheAdd ( int iBinlog, BinlogReader_c & tReader ) const;
	bool					ReplayReconfigure ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const;
};


struct SphChunkGuard_t
{
	CSphFixedVector<const RtSegment_t *>	m_dRamChunks { 0 };
	CSphFixedVector<const CSphIndex *>		m_dDiskChunks { 0 };
	CSphFixedVector<const KlistRefcounted_t *>		m_dKill { 0 };
	CSphRwlock *							m_pReading = nullptr;
	~SphChunkGuard_t();
};


struct ChunkStats_t
{
	CSphSourceStats				m_Stats;
	CSphFixedVector<int64_t>	m_dFieldLens;

	explicit ChunkStats_t ( const CSphSourceStats & s, const CSphFixedVector<int64_t> & dLens )
		: m_dFieldLens ( dLens.GetLength() )
	{
		m_Stats = s;
		ARRAY_FOREACH ( i, dLens )
			m_dFieldLens[i] = dLens[i];
	}
};

template<typename VEC>
CSphFixedVector<int> GetIndexNames ( const VEC & dIndexes, bool bAddNext )
{
	CSphFixedVector<int> dNames ( dIndexes.GetLength() + ( bAddNext ? 1 : 0 ) );

	if ( !dIndexes.GetLength() )
	{
		if ( bAddNext )
			dNames[0] = 0;

		return dNames;
	}

	int iLast = 0;
	ARRAY_FOREACH ( iChunk, dIndexes )
	{
		const char * sName = dIndexes[iChunk]->GetFilename();
		assert ( sName );
		int iLen = strlen ( sName );
		assert ( iLen > 0 );

		const char * sNum = sName + iLen - 1;
		while ( sNum && *sNum && *sNum>='0' && *sNum<='9' )
			sNum--;

		iLast = atoi(sNum+1);
		dNames[iChunk] = iLast;
	}

	if ( bAddNext )
		dNames[dIndexes.GetLength()] = iLast + 1;

	return dNames;
}

/// RAM based index
struct RtQword_t;
struct RtIndex_t : public ISphRtIndex, public ISphNoncopyable, public ISphWordlist, public ISphWordlistSuggest
{
private:
	static const DWORD			META_HEADER_MAGIC	= 0x54525053;	///< my magic 'SPRT' header
	static const DWORD			META_VERSION		= 13;			///< current version

private:
	int							m_iStride;
	CSphVector<RtSegment_t*>	m_dRamChunks;
	CSphVector<const RtSegment_t*>	m_dRetired;

	CSphMutex					m_tWriting;
	mutable CSphRwlock			m_tChunkLock;
	mutable CSphRwlock			m_tReading;

	/// double buffer stuff (allows to work with RAM chunk while future disk is being saved)
	/// m_dSegments consists of two parts
	/// segments with indexes < m_iDoubleBuffer are being saved now as a disk chunk
	/// segments with indexes >= m_iDoubleBuffer are RAM chunk
	CSphMutex					m_tFlushLock;
	CSphMutex					m_tOptimizingLock;
	int							m_iDoubleBuffer;
	CSphVector<SphDocID_t>		m_dNewSegmentKlist;					///< raw docid container
	CSphVector<SphDocID_t>		m_dDiskChunkKlist;					///< ordered SphDocID_t kill list

	int64_t						m_iSoftRamLimit;
	int64_t						m_iDoubleBufferLimit;
	CSphString					m_sPath;
	bool						m_bPathStripped;
	CSphVector<CSphIndex*>		m_dDiskChunks GUARDED_BY ( m_tChunkLock );
	int							m_iLockFD;
	mutable CSphKilllist		m_tKlist;							///< kill list for disk chunks and saved chunks
	volatile bool				m_bOptimizing;
	volatile bool				m_bOptimizeStop;

	int64_t						m_iSavedTID;
	int64_t						m_tmSaved;
	mutable DWORD				m_uDiskAttrStatus;

	bool						m_bKeywordDict;
	int							m_iWordsCheckpoint;
	int							m_iMaxCodepointLength;
	ISphTokenizerRefPtr_c		m_pTokenizerIndexing;
	bool						m_bLoadRamPassedOk;

	bool						m_bMlock;
	bool						m_bOndiskAllAttr;
	bool						m_bOndiskPoolAttr;

	CSphFixedVector<int64_t>	m_dFieldLens;						///< total field lengths over entire index
	CSphFixedVector<int64_t>	m_dFieldLensRam;					///< field lengths summed over current RAM chunk
	CSphFixedVector<int64_t>	m_dFieldLensDisk;					///< field lengths summed over all disk chunks
	CSphBitvec					m_tMorphFields;

public:
	explicit					RtIndex_t ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict );
	virtual						~RtIndex_t ();

	virtual bool				AddDocument ( ISphTokenizer * pTokenizer, int iFields, const char ** ppFields, const CSphMatch & tDoc, bool bReplace, const CSphString & sTokenFilterOptions, const char ** ppStr, const CSphVector<DWORD> & dMvas, CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt );
	virtual bool				AddDocument ( ISphHits * pHits, const CSphMatch & tDoc, bool bReplace, const char ** ppStr, const CSphVector<DWORD> & dMvas, CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt );
	virtual bool				DeleteDocument ( const SphDocID_t * pDocs, int iDocs, CSphString & sError, ISphRtAccum * pAccExt );
	virtual void				Commit ( int * pDeleted, ISphRtAccum * pAccExt );
	virtual void				RollBack ( ISphRtAccum * pAccExt );
	void						CommitReplayable ( RtSegment_t * pNewSeg, CSphVector<SphDocID_t> & dAccKlist, int * pTotalKilled ); // FIXME? protect?
	virtual void				CheckRamFlush ();
	virtual void				ForceRamFlush ( bool bPeriodic=false );
	virtual void				ForceDiskChunk ();
	virtual bool				AttachDiskIndex ( CSphIndex * pIndex, CSphString & sError );
	virtual bool				Truncate ( CSphString & sError );
	virtual void				Optimize ();
	virtual void				ProgressiveMerge ();
	CSphIndex *					GetDiskChunk ( int iChunk ) { return m_dDiskChunks.GetLength()>iChunk ? m_dDiskChunks[iChunk] : NULL; }
	virtual ISphTokenizer *		CloneIndexingTokenizer() const { return m_pTokenizerIndexing->Clone ( SPH_CLONE_INDEX ); }

private:
	virtual ISphRtAccum *		CreateAccum ( CSphString & sError );

	RtSegment_t *				MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2, const CSphVector<SphDocID_t> * pAccKlist, bool bHasMorphology );
	const RtWord_t *			CopyWord ( RtSegment_t * pDst, RtWordWriter_t & tOutWord, const RtSegment_t * pSrc, const RtWord_t * pWord, RtWordReader_t & tInWord, const CSphVector<SphDocID_t> * pAccKlist );
	void						MergeWord ( RtSegment_t * pDst, const RtSegment_t * pSrc1, const RtWord_t * pWord1, const RtSegment_t * pSrc2, const RtWord_t * pWord2, RtWordWriter_t & tOut, const CSphVector<SphDocID_t> * pAccKlist );
	void						CopyDoc ( RtSegment_t * pSeg, RtDocWriter_t & tOutDoc, RtWord_t * pWord, const RtSegment_t * pSrc, const RtDoc_t * pDoc );

	void						SaveMeta ( int64_t iTID, const CSphFixedVector<int> & dChunkNames );
	void						SaveDiskHeader ( const char * sFilename, SphDocID_t iMinDocID, int iCheckpoints, SphOffset_t iCheckpointsPosition, DWORD iInfixBlocksOffset, int iInfixCheckpointWordsSize, DWORD uKillListSize, uint64_t uMinMaxSize, const ChunkStats_t & tStats, int64_t iTotalDocuments ) const;
	void						SaveDiskDataImpl ( const char * sFilename, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats ) const;
	void						SaveDiskChunk ( int64_t iTID, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats, bool bMoveRetired );
	CSphIndex *					LoadDiskChunk ( const char * sChunk, CSphString & sError ) const;
	bool						LoadRamChunk ( DWORD uVersion, bool bRebuildInfixes );
	bool						SaveRamChunk ();

	virtual void				GetPrefixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const;
	virtual void				GetInfixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const;
	virtual void				GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const;

	virtual void				SuffixGetChekpoints ( const SuggestResult_t & tRes, const char * sSuffix, int iLen, CSphVector<DWORD> & dCheckpoints ) const;
	virtual void				SetCheckpoint ( SuggestResult_t & tRes, DWORD iCP ) const;
	virtual bool				ReadNextWord ( SuggestResult_t & tRes, DictWord_t & tWord ) const;

public:
#if USE_WINDOWS
#pragma warning(push,1)
#pragma warning(disable:4100)
#endif
	virtual SphDocID_t *		GetKillList () const				{ return NULL; }
	virtual int					GetKillListSize () const			{ return 0; }
	virtual bool				HasDocid ( SphDocID_t ) const		{ assert ( 0 ); return false; }

	virtual int					Build ( const CSphVector<CSphSource*> & , int , int ) { return 0; }
	virtual bool				Merge ( CSphIndex * , const CSphVector<CSphFilterSettings> & , bool ) { return false; }

	virtual bool				Prealloc ( bool bStripPath );
	virtual void				Dealloc () {}
	virtual void				Preread ();
	virtual void				SetMemorySettings ( bool bMlock, bool bOndiskAttrs, bool bOndiskPool );
	virtual void				SetBase ( const char * ) {}
	virtual bool				Rename ( const char * ) { return true; }
	virtual bool				Lock () { return true; }
	virtual void				Unlock () {}
	virtual void				PostSetup();
	virtual bool				IsRT() const { return true; }

	virtual int					UpdateAttributes ( const CSphAttrUpdate & tUpd, int iIndex, CSphString & sError, CSphString & sWarning );
	virtual bool				SaveAttributes ( CSphString & sError ) const;
	virtual DWORD				GetAttributeStatus () const { return m_uDiskAttrStatus; }
	virtual bool				AddRemoveAttribute ( bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError );

	virtual void				DebugDumpHeader ( FILE * , const char * , bool ) {}
	virtual void				DebugDumpDocids ( FILE * ) {}
	virtual void				DebugDumpHitlist ( FILE * , const char * , bool ) {}
	virtual void				DebugDumpDict ( FILE * ) {}
	virtual int					DebugCheck ( FILE * fp );
#if USE_WINDOWS
#pragma warning(pop)
#endif

public:
	virtual bool						EarlyReject ( CSphQueryContext * pCtx, CSphMatch & ) const;
	virtual const CSphSourceStats &		GetStats () const { return m_tStats; }
	virtual int64_t *					GetFieldLens() const { return m_tSettings.m_bIndexFieldLens ? m_dFieldLens.Begin() : nullptr; }
	virtual void				GetStatus ( CSphIndexStatus* ) const;

	virtual bool				MultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const;
	virtual bool				MultiQueryEx ( int iQueries, const CSphQuery * ppQueries, CSphQueryResult ** ppResults, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const;
	bool						DoGetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, const GetKeywordsSettings_t & tSettings, bool bFillOnly, CSphString * pError, const SphChunkGuard_t & tGuard ) const;
	virtual bool				GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, const GetKeywordsSettings_t & tSettings, CSphString * pError ) const;
	virtual bool				FillKeywords ( CSphVector <CSphKeywordInfo> & dKeywords ) const;
	void						AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, CSphDict * pDict, bool bGetStats, int iQpos, RtQword_t * pQueryWord, CSphVector <CSphKeywordInfo> & dKeywords, const SphChunkGuard_t & tGuard ) const;

	bool						RtQwordSetup ( RtQword_t * pQword, int iSeg, const SphChunkGuard_t & tGuard ) const;
	static bool					RtQwordSetupSegment ( RtQword_t * pQword, const RtSegment_t * pSeg, bool bSetup, bool bWordDict, int iWordsCheckpoint, const CSphFixedVector<SphDocID_t> & dKill, const CSphIndexSettings & tSettings );

	virtual bool				IsStarDict() const;

	virtual const CSphSchema &	GetMatchSchema () const { return m_tSchema; }
	virtual const CSphSchema &	GetInternalSchema () const { return m_tSchema; }
	int64_t						GetUsedRam () const;

	bool						IsWordDict () const { return m_bKeywordDict; }
	int							GetWordCheckoint() const { return m_iWordsCheckpoint; }
	int							GetMaxCodepointLength() const { return m_iMaxCodepointLength; }

	// TODO: implement me
	virtual	void				SetProgressCallback ( CSphIndexProgress::IndexingProgress_fn ) {}

	virtual bool				IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError ) const;
	virtual void				Reconfigure ( CSphReconfigureSetup & tSetup );
	virtual int64_t				GetFlushAge() const override;

protected:
	CSphSourceStats				m_tStats;

private:

	void						GetReaderChunks ( SphChunkGuard_t & tGuard ) const;
	void						FreeRetired();
};


RtIndex_t::RtIndex_t ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict )

	: ISphRtIndex ( sIndexName, sPath )
	, m_dDiskChunkKlist ( 0 )
	, m_iSoftRamLimit ( iRamSize )
	, m_sPath ( sPath )
	, m_bPathStripped ( false )
	, m_iLockFD ( -1 )
	, m_bOptimizing ( false )
	, m_bOptimizeStop ( false )
	, m_iSavedTID ( m_iTID )
	, m_tmSaved ( sphMicroTimer() )
	, m_uDiskAttrStatus ( 0 )
	, m_bKeywordDict ( bKeywordDict )
	, m_iWordsCheckpoint ( RTDICT_CHECKPOINT_V5 )
	, m_iMaxCodepointLength ( 0 )
	, m_dFieldLens ( SPH_MAX_FIELDS )
	, m_dFieldLensRam ( SPH_MAX_FIELDS )
	, m_dFieldLensDisk ( SPH_MAX_FIELDS )
{
	MEMORY ( MEM_INDEX_RT );

	m_tSchema = tSchema;
	m_iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();

	m_iDoubleBufferLimit = ( m_iSoftRamLimit * SPH_RT_DOUBLE_BUFFER_PERCENT ) / 100;
	m_iDoubleBuffer = 0;
	m_bMlock = false;
	m_bOndiskAllAttr = false;
	m_bOndiskPoolAttr = false;
	m_bLoadRamPassedOk = true;

#ifndef NDEBUG
	// check that index cols are static
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		assert ( !m_tSchema.GetAttr(i).m_tLocator.m_bDynamic );
#endif

	Verify ( m_tChunkLock.Init() );
	Verify ( m_tReading.Init() );

	ARRAY_FOREACH ( i, m_dFieldLens )
	{
		m_dFieldLens[i] = 0;
		m_dFieldLensRam[i] = 0;
		m_dFieldLensDisk[i] = 0;
	}
}


RtIndex_t::~RtIndex_t ()
{
	int64_t tmSave = sphMicroTimer();
	bool bValid = m_pTokenizer && m_pDict && m_bLoadRamPassedOk;

	if ( bValid )
	{
		SaveRamChunk ();
		CSphFixedVector<int> dNames = GetIndexNames ( m_dDiskChunks, false );
		SaveMeta ( m_iTID, dNames );
	}

	Verify ( m_tReading.Done() );
	Verify ( m_tChunkLock.Done() );

	for ( auto & dRamChunk : m_dRamChunks )
		SafeDelete ( dRamChunk );

	m_dRetired.Uniq();
	for ( auto & dRetired : m_dRetired )
		SafeDelete ( dRetired );

	for ( auto & dDiskChunk : m_dDiskChunks )
		SafeDelete ( dDiskChunk );

	if ( m_iLockFD>=0 )
		::close ( m_iLockFD );

	// might be NULL during startup
	if ( g_pBinlog )
		g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, true );

	tmSave = sphMicroTimer() - tmSave;
	if ( tmSave>=1000 && bValid )
	{
		sphInfo ( "rt: index %s: ramchunk saved in %d.%03d sec",
			m_sIndexName.cstr(), (int)(tmSave/1000000), (int)((tmSave/1000)%1000) );
	}
}


static int64_t g_iRtFlushPeriod = 10*60*60; // default period is 10 hours

void RtIndex_t::CheckRamFlush ()
{
	if ( ( sphMicroTimer()-m_tmSaved )/1000000<g_iRtFlushPeriod )
		return;
	if ( g_pRtBinlog->IsActive() && m_iTID<=m_iSavedTID )
		return;

	ForceRamFlush ( true );
}


void RtIndex_t::ForceRamFlush ( bool bPeriodic )
{
	int64_t tmSave = sphMicroTimer();

	// need this lock as could get here at same time either ways:
	// via RtFlushThreadFunc->RtIndex_t::CheckRamFlush
	// and via HandleMysqlFlushRtindex
	CSphScopedLock<CSphMutex> tLock ( m_tFlushLock );

	if ( g_pRtBinlog->IsActive() && m_iTID<=m_iSavedTID )
		return;

	Verify ( m_tWriting.Lock() );

	int64_t iUsedRam = GetUsedRam();
	if ( !SaveRamChunk () )
	{
		sphWarning ( "rt: index %s: ramchunk save FAILED! (error=%s)", m_sIndexName.cstr(), m_sLastError.cstr() );
		Verify ( m_tWriting.Unlock() );
		return;
	}
	CSphFixedVector<int> dNames = GetIndexNames ( m_dDiskChunks, false );
	SaveMeta ( m_iTID, dNames );
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	int64_t iWasTID = m_iSavedTID;
	int64_t tmDelta = sphMicroTimer() - m_tmSaved;
	m_iSavedTID = m_iTID;
	m_tmSaved = sphMicroTimer();

	Verify ( m_tWriting.Unlock() );

	tmSave = sphMicroTimer() - tmSave;
	sphInfo ( "rt: index %s: ramchunk saved ok (mode=%s, last TID=" INT64_FMT ", current TID=" INT64_FMT ", "
		"ram=%d.%03d Mb, time delta=%d sec, took=%d.%03d sec)"
		, m_sIndexName.cstr(), bPeriodic ? "periodic" : "forced"
		, iWasTID, m_iTID, (int)(iUsedRam/1024/1024), (int)((iUsedRam/1024)%1000)
		, (int) (tmDelta/1000000), (int)(tmSave/1000000), (int)((tmSave/1000)%1000) );
}


int64_t RtIndex_t::GetFlushAge() const
{
	if ( m_iSavedTID==0 || m_iSavedTID==m_iTID )
		return 0;

	return m_tmSaved;
}


int64_t RtIndex_t::GetUsedRam () const
{
	int64_t iTotal = 0;
	ARRAY_FOREACH ( i, m_dRamChunks )
		iTotal += m_dRamChunks[i]->GetUsedRam();

	return iTotal;
}

//////////////////////////////////////////////////////////////////////////
// INDEXING
//////////////////////////////////////////////////////////////////////////

class CSphSource_StringVector : public CSphSource_Document
{
public:
	explicit			CSphSource_StringVector ( int iFields, const char ** ppFields, const CSphSchema & tSchema );
						~CSphSource_StringVector () override = default;

	bool		Connect ( CSphString & ) override;
	void		Disconnect () override;

	bool HasAttrsConfigured () override { return false; }
	bool IterateStart ( CSphString & ) override { m_iPlainFieldsLength = m_tSchema.GetFieldsCount(); return true; }

	bool		IterateMultivaluedStart ( int, CSphString & ) override { return false; }
	bool		IterateMultivaluedNext () override { return false; }

	SphRange_t			IterateFieldMVAStart ( int ) override { return {}; }

	bool		IterateKillListStart ( CSphString & ) override { return false; }
	bool		IterateKillListNext ( SphDocID_t & ) override { return false; }

	BYTE **		NextDocument ( CSphString & ) override { return m_dFields.Begin(); }
	const int *	GetFieldLengths () const override { return m_dFieldLengths.Begin(); }
	void		SetMorphFields ( const CSphBitvec & tMorphFields ) { m_tMorphFields = tMorphFields; }

protected:
	CSphVector<BYTE *>			m_dFields;
	CSphVector<int>				m_dFieldLengths;
};


CSphSource_StringVector::CSphSource_StringVector ( int iFields, const char ** ppFields, const CSphSchema & tSchema )
	: CSphSource_Document ( "$stringvector" )
{
	m_tSchema = tSchema;

	m_dFields.Resize ( 1+iFields );
	m_dFieldLengths.Resize ( iFields );
	for ( int i=0; i<iFields; i++ )
	{
		m_dFields[i] = (BYTE*) ppFields[i];
		m_dFieldLengths[i] = strlen ( ppFields[i] );
		assert ( m_dFields[i] );
	}
	m_dFields [ iFields ] = NULL;

	m_iMaxHits = 0; // force all hits build
}

bool CSphSource_StringVector::Connect ( CSphString & )
{
	// no AddAutoAttrs() here; they should already be in the schema
	m_tHits.m_dData.Reserve ( 1024 );
	return true;
}

void CSphSource_StringVector::Disconnect ()
{
	m_tHits.m_dData.Reset();
}

bool RtIndex_t::AddDocument ( ISphTokenizer * pTokenizer, int iFields, const char ** ppFields, const CSphMatch & tDoc,
	bool bReplace, const CSphString & sTokenFilterOptions,
	const char ** ppStr, const CSphVector<DWORD> & dMvas,
	CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	ISphTokenizerRefPtr_c tTokenizer { pTokenizer };

	if ( !tDoc.m_uDocID )
		return true;

	MEMORY ( MEM_INDEX_RT );

	if ( !bReplace )
	{
		CSphScopedRLock rLock { m_tChunkLock };
		for ( auto& dRamChunk : m_dRamChunks )
			if ( dRamChunk->FindAliveRow ( tDoc.m_uDocID) )
			{
				sError.SetSprintf ( "duplicate id '" UINT64_FMT "'", (uint64_t)tDoc.m_uDocID );
				return false; // already exists and not deleted; INSERT fails
			}
	}

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict, true, &sError );
	if ( !pAcc )
		return false;

	// OPTIMIZE? do not create filter on each(!) INSERT
	if ( !m_tSettings.m_sIndexTokenFilter.IsEmpty() )
	{
		tTokenizer = ISphTokenizer::CreatePluginFilter ( tTokenizer, m_tSettings.m_sIndexTokenFilter, sError );
		if ( !tTokenizer )
			return false;
		if ( !tTokenizer->SetFilterSchema ( m_tSchema, sError ) )
			return false;
		if ( !sTokenFilterOptions.IsEmpty() )
			if ( !tTokenizer->SetFilterOptions ( sTokenFilterOptions.cstr(), sError ) )
				return false;
	}

	// OPTIMIZE? do not create filter on each(!) INSERT
	if ( m_tSettings.m_uAotFilterMask )
		tTokenizer = sphAotCreateFilter ( tTokenizer, m_pDict, m_tSettings.m_bIndexExactWords, m_tSettings.m_uAotFilterMask );

	CSphSource_StringVector tSrc ( iFields, ppFields, m_tSchema );

	// SPZ setup
	if ( m_tSettings.m_bIndexSP && !tTokenizer->EnableSentenceIndexing ( sError ) )
		return false;

	if ( !m_tSettings.m_sZones.IsEmpty() && !tTokenizer->EnableZoneIndexing ( sError ) )
		return false;

	if ( m_tSettings.m_bHtmlStrip && !tSrc.SetStripHTML ( m_tSettings.m_sHtmlIndexAttrs.cstr(), m_tSettings.m_sHtmlRemoveElements.cstr(),
			m_tSettings.m_bIndexSP, m_tSettings.m_sZones.cstr(), sError ) )
		return false;

	// OPTIMIZE? do not clone filters on each INSERT
	ISphFieldFilterRefPtr_c pFieldFilter;
	if ( m_pFieldFilter )
		pFieldFilter = m_pFieldFilter->Clone();

	tSrc.Setup ( m_tSettings );
	tSrc.SetTokenizer ( tTokenizer );
	tSrc.SetDict ( pAcc->m_pDict );
	tSrc.SetFieldFilter ( pFieldFilter );
	tSrc.SetMorphFields ( m_tMorphFields );
	if ( !tSrc.Connect ( m_sLastError ) )
		return false;

	m_tSchema.CloneWholeMatch ( &tSrc.m_tDocInfo, tDoc );

	if ( !tSrc.IterateStart ( sError ) || !tSrc.IterateDocument ( sError ) )
		return false;

	ISphHits * pHits = tSrc.IterateHits ( sError );
	pAcc->GrabLastWarning ( sWarning );

	if ( !AddDocument ( pHits, tDoc, bReplace, ppStr, dMvas, sError, sWarning, pAcc ) )
		return false;

	m_tStats.m_iTotalBytes += tSrc.GetStats().m_iTotalBytes;

	return true;
}


static void AccumCleanup ( void * pArg )
{
	auto pAcc = (RtAccum_t *) pArg;
	SafeDelete ( pAcc );
}


ISphRtAccum * ISphRtIndex::AcquireAccum ( CSphDict * pDict, ISphRtAccum * pAccExt,
	bool bWordDict, bool bSetTLS, CSphString* sError )
{
	auto pAcc = ( RtAccum_t * ) ( pAccExt ? pAccExt : sphThreadGet ( g_tTlsAccumKey ) );

	if ( pAcc && pAcc->GetIndex() && pAcc->GetIndex()!=this )
	{
		if ( sError )
			sError->SetSprintf ( "current txn is working with another index ('%s')", pAcc->GetIndex()->GetName() );
		return nullptr;
	}

	if ( !pAcc )
	{
		pAcc = new RtAccum_t ( bWordDict );
		if ( bSetTLS )
		{
			sphThreadSet ( g_tTlsAccumKey, pAcc );
			sphThreadOnExit ( AccumCleanup, pAcc );
		}
	}

	assert ( pAcc->GetIndex()==nullptr || pAcc->GetIndex()==this );
	pAcc->SetIndex ( this );
	pAcc->SetupDict ( this, pDict, bWordDict );
	return pAcc;
}

ISphRtAccum * RtIndex_t::CreateAccum ( CSphString & sError )
{
	return AcquireAccum ( m_pDict, nullptr, m_bKeywordDict, false, &sError);
}


bool RtIndex_t::AddDocument ( ISphHits * pHits, const CSphMatch & tDoc, bool bReplace, const char ** ppStr, const CSphVector<DWORD> & dMvas,
	CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	RtAccum_t * pAcc = (RtAccum_t *)pAccExt;

	if ( pAcc )
		pAcc->AddDocument ( pHits, tDoc, bReplace, m_tSchema.GetRowSize(), ppStr, dMvas );

	return ( pAcc!=NULL );
}


RtAccum_t::RtAccum_t ( bool bKeywordDict )
	: m_bKeywordDict ( bKeywordDict )
{
	m_dStrings.Add ( 0 );
	m_dMvas.Add ( 0 );
}
void RtAccum_t::SetupDict ( const ISphRtIndex * pIndex, CSphDict * pDict, bool bKeywordDict )
{
	if ( pIndex!=m_pIndex || pDict!=m_pRefDict || bKeywordDict!=m_bKeywordDict )
	{
		m_bKeywordDict = bKeywordDict;
		m_pRefDict = pDict;
		m_pDict = GetStatelessDict ( pDict );
		if ( m_bKeywordDict )
		{
			m_pDict = m_pDictRt = sphCreateRtKeywordsDictionaryWrapper ( m_pDict );
			SafeAddRef ( m_pDict ); // since m_pDict and m_pDictRt are DIFFERENT types, = works via CsphDict*
		}
	}
}

void RtAccum_t::ResetDict ()
{
	assert ( !m_bKeywordDict || m_pDictRt );
	if ( m_pDictRt )
	{
		m_pDictRt->ResetKeywords();
	}
}

void RtAccum_t::Sort ()
{
	if ( !m_bKeywordDict )
	{
		m_dAccum.Sort ( CmpHitPlain_fn() );
	} else
	{
		assert ( m_pDictRt );
		const BYTE * pPackedKeywords = m_pDictRt->GetPackedKeywords();
		m_dAccum.Sort ( CmpHitKeywords_fn ( pPackedKeywords ) );
	}
}

void RtAccum_t::Cleanup ( BYTE eWhat )
{
	if ( eWhat & EPartial )
	{
		m_dAccumRows.Resize ( 0 );
		m_dStrings.Resize ( 1 ); // handle dummy zero offset
		m_dMvas.Resize ( 1 );
		m_dPerDocHitsCount.Resize ( 0 );
		ResetDict ();
	}
	if ( eWhat & EAccum )
		m_dAccum.Resize ( 0 );
	if ( eWhat & ERest )
	{
		SetIndex ( nullptr );
		m_iAccumDocs = 0;
		m_dAccumKlist.Reset ();
	}
}

void RtAccum_t::AddDocument ( ISphHits * pHits, const CSphMatch & tDoc, bool bReplace, int iRowSize, const char ** ppStr, const CSphVector<DWORD> & dMvas )
{
	MEMORY ( MEM_RT_ACCUM );

	// FIXME? what happens on mixed insert/replace?
	m_bReplace = bReplace;

	// schedule existing copies for deletion
	m_dAccumKlist.Add ( tDoc.m_uDocID );

	// reserve some hit space on first use
	if ( pHits && pHits->Length() && !m_dAccum.GetLength() )
		m_dAccum.Reserve ( 128*1024 );

	// accumulate row data; expect fully dynamic rows
	assert ( !tDoc.m_pStatic );
	assert (!( !tDoc.m_pDynamic && iRowSize!=0 ));
	assert (!( tDoc.m_pDynamic && (int)tDoc.m_pDynamic[-1]!=iRowSize ));

	m_dAccumRows.Resize ( m_dAccumRows.GetLength() + DOCINFO_IDSIZE + iRowSize );
	CSphRowitem * pRow = &m_dAccumRows [ m_dAccumRows.GetLength() - DOCINFO_IDSIZE - iRowSize ];
	DOCINFOSETID ( pRow, tDoc.m_uDocID );

	CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);
	for ( int i=0; i<iRowSize; i++ )
		pAttrs[i] = tDoc.m_pDynamic[i];

	int iMva = 0;

	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();
	int iAttr = 0;
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tColumn = tSchema.GetAttr(i);
		if ( tColumn.m_eAttrType==SPH_ATTR_STRING || tColumn.m_eAttrType==SPH_ATTR_JSON )
		{
			const char * pStr = ppStr ? ppStr[iAttr++] : nullptr;
			int iLen = 0;
			if ( tColumn.m_eAttrType==SPH_ATTR_STRING )
			{
				iLen = ( pStr ? strlen ( pStr ) : 0 );
			} else if ( pStr ) // SPH_ATTR_JSON - len: 4bytes + data
			{
				iLen = sphUnpackStr ( (const BYTE *)pStr, nullptr );
				pStr += 4;
			}

			if ( pStr && iLen )
			{
				BYTE dLen[3];
				const int iLenPacked = sphPackStrlen ( dLen, iLen );
				const int iOff = m_dStrings.GetLength();
				assert ( iOff>=1 );
				m_dStrings.Resize ( iOff + iLenPacked + iLen );
				memcpy ( &m_dStrings[iOff], dLen, iLenPacked );
				memcpy ( &m_dStrings[iOff+iLenPacked], pStr, iLen );
				sphSetRowAttr ( pAttrs, tColumn.m_tLocator, iOff );
			} else
			{
				sphSetRowAttr ( pAttrs, tColumn.m_tLocator, 0 );
			}
		} else if ( tColumn.m_eAttrType==SPH_ATTR_UINT32SET || tColumn.m_eAttrType==SPH_ATTR_INT64SET )
		{
			assert ( m_dMvas.GetLength() );
			int iCount = dMvas[iMva];
			if ( iCount )
			{
				int iDst = m_dMvas.GetLength();
				m_dMvas.Resize ( iDst+iCount+1 );
				memcpy ( m_dMvas.Begin()+iDst, dMvas.Begin()+iMva, (iCount+1)*sizeof(dMvas[0]) );
				sphSetRowAttr ( pAttrs, tColumn.m_tLocator, iDst );
			} else
			{
				sphSetRowAttr ( pAttrs, tColumn.m_tLocator, 0 );
			}

			iMva += iCount+1;
		}
	}

	// handle index_field_lengths
	DWORD * pFieldLens = NULL;
	if ( m_pIndex->GetSettings().m_bIndexFieldLens )
	{
		int iFirst = tSchema.GetAttrId_FirstFieldLen();
		assert ( tSchema.GetAttr ( iFirst ).m_eAttrType==SPH_ATTR_TOKENCOUNT );
		assert ( tSchema.GetAttr ( iFirst+tSchema.GetFieldsCount()-1 ).m_eAttrType==SPH_ATTR_TOKENCOUNT );
		pFieldLens = pAttrs + ( tSchema.GetAttr ( iFirst ).m_tLocator.m_iBitOffset / 32 );
		memset ( pFieldLens, 0, sizeof(int)*tSchema.GetFieldsCount() ); // NOLINT
	}

	// accumulate hits
	int iHits = 0;
	if ( pHits && pHits->Length() )
	{
		CSphWordHit tLastHit;
		tLastHit.m_uDocID = 0;
		tLastHit.m_uWordID = 0;
		tLastHit.m_uWordPos = 0;

		iHits = pHits->Length();
		m_dAccum.Reserve ( m_dAccum.GetLength()+iHits );
		iHits = 0;
		for ( CSphWordHit * pHit = pHits->m_dData.Begin(); pHit<=pHits->Last(); pHit++ )
		{
			// ignore duplicate hits
			if ( pHit->m_uDocID==tLastHit.m_uDocID && pHit->m_uWordID==tLastHit.m_uWordID && pHit->m_uWordPos==tLastHit.m_uWordPos )
				continue;

			// update field lengths
			if ( pFieldLens && HITMAN::GetField ( pHit->m_uWordPos )!=HITMAN::GetField ( tLastHit.m_uWordPos ) )
				pFieldLens [ HITMAN::GetField ( tLastHit.m_uWordPos ) ] = HITMAN::GetPos ( tLastHit.m_uWordPos );

			// need original hit for duplicate removal
			tLastHit = *pHit;
			// reset field end for not very last position
			if ( HITMAN::IsEnd ( pHit->m_uWordPos ) && pHit!=pHits->Last() &&
				pHit->m_uDocID==pHit[1].m_uDocID && pHit->m_uWordID==pHit[1].m_uWordID && HITMAN::IsEnd ( pHit[1].m_uWordPos ) )
				pHit->m_uWordPos = HITMAN::GetPosWithField ( pHit->m_uWordPos );

			// accumulate
			m_dAccum.Add ( *pHit );
			iHits++;
		}
		if ( pFieldLens )
			pFieldLens [ HITMAN::GetField ( tLastHit.m_uWordPos ) ] = HITMAN::GetPos ( tLastHit.m_uWordPos );
	}
	// make sure to get real count without duplicated hits
	m_dPerDocHitsCount.Add ( iHits );

	m_iAccumDocs++;
}


// cook checkpoints - make NULL terminating strings from offsets
static void FixupSegmentCheckpoints ( RtSegment_t * pSeg )
{
	assert ( pSeg &&
		( !pSeg->m_dWordCheckpoints.GetLength() || pSeg->m_dKeywordCheckpoints.GetLength() ) );
	if ( !pSeg->m_dWordCheckpoints.GetLength() )
		return;

	const char * pBase = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
	assert ( pBase );
	for ( auto & dCheckpoint : pSeg->m_dWordCheckpoints )
		dCheckpoint.m_sWord = pBase + dCheckpoint.m_uWordID;
}


RtSegment_t * RtAccum_t::CreateSegment ( int iRowSize, int iWordsCheckpoint )
{
	if ( !m_iAccumDocs )
		return nullptr;

	MEMORY ( MEM_RT_ACCUM );

	auto * pSeg = new RtSegment_t ();

	m_dAccum.Add ( CSphWordHit() );

	RtDoc_t tDoc;
	RtWord_t tWord;
	RtDocWriter_t tOutDoc ( pSeg );
	RtWordWriter_t tOutWord ( pSeg, m_bKeywordDict, iWordsCheckpoint );
	RtHitWriter_t tOutHit ( pSeg );

	const BYTE * pPacketBase = m_bKeywordDict ? m_pDictRt->GetPackedKeywords() : nullptr;

	Hitpos_t uEmbeddedHit = EMPTY_HIT;
	Hitpos_t uPrevHit = EMPTY_HIT;

	for ( const CSphWordHit &tHit : m_dAccum )
	{
		// new keyword or doc; flush current doc
		if ( tHit.m_uWordID!=tWord.m_uWordID || tHit.m_uDocID!=tDoc.m_uDocID )
		{
			if ( tDoc.m_uDocID )
			{
				++tWord.m_uDocs;
				tWord.m_uHits += tDoc.m_uHits;

				if ( uEmbeddedHit )
				{
					assert ( tDoc.m_uHits==1 );
					tDoc.m_uHit = uEmbeddedHit;
				}

				tOutDoc.ZipDoc ( tDoc );
				tDoc.m_uDocFields = 0;
				tDoc.m_uHits = 0;
				tDoc.m_uHit = tOutHit.ZipHitPtr();
			}

			tDoc.m_uDocID = tHit.m_uDocID;
			tOutHit.ZipRestart ();
			uEmbeddedHit = EMPTY_HIT;
			uPrevHit = EMPTY_HIT;
		}

		// new keyword; flush current keyword
		if ( tHit.m_uWordID!=tWord.m_uWordID )
		{
			tOutDoc.ZipRestart ();
			if ( tWord.m_uWordID )
			{
				if ( m_bKeywordDict )
				{
					const BYTE * pPackedWord = pPacketBase + tWord.m_uWordID;
					assert ( pPackedWord[0] && pPackedWord[0]+1<m_pDictRt->GetPackedLen() );
					tWord.m_sWord = pPackedWord;
				}
				tOutWord.ZipWord ( tWord );
			}

			tWord.m_uWordID = tHit.m_uWordID;
			tWord.m_uDocs = 0;
			tWord.m_uHits = 0;
			tWord.m_uDoc = tOutDoc.ZipDocPtr();
			uPrevHit = EMPTY_HIT;
		}

		// might be a duplicate
		if ( uPrevHit==tHit.m_uWordPos )
			continue;

		// just a new hit
		if ( !tDoc.m_uHits )
		{
			uEmbeddedHit = tHit.m_uWordPos;
		} else
		{
			if ( uEmbeddedHit )
			{
				tOutHit.ZipHit ( uEmbeddedHit );
				uEmbeddedHit = 0;
			}

			tOutHit.ZipHit ( tHit.m_uWordPos );
		}
		uPrevHit = tHit.m_uWordPos;

		const int iField = HITMAN::GetField ( tHit.m_uWordPos );
		if ( iField<32 )
			tDoc.m_uDocFields |= ( 1UL<<iField );
		++tDoc.m_uHits;
	}

	if ( m_bKeywordDict )
		FixupSegmentCheckpoints ( pSeg );

	pSeg->m_iRows = m_iAccumDocs;
	pSeg->m_iAliveRows = m_iAccumDocs;

	// copy and sort attributes
	int iStride = DOCINFO_IDSIZE + iRowSize;
	pSeg->m_dRows.SwapData ( m_dAccumRows );
	pSeg->m_dStrings.SwapData ( m_dStrings );
	pSeg->m_dMvas.SwapData ( m_dMvas );
	sphSortDocinfos ( pSeg->m_dRows.Begin(), pSeg->m_dRows.GetLength()/iStride, iStride );

	// done
	return pSeg;
}


struct AccumDocHits_t
{
	SphDocID_t m_uDocid;
	int m_iDocIndex;
	int m_iHitIndex;
	int m_iHitCount;
};


struct CmpDocHitIndex_t
{
	inline bool IsLess ( const AccumDocHits_t & a, const AccumDocHits_t & b ) const
	{
		return ( a.m_uDocid<b.m_uDocid || ( a.m_uDocid==b.m_uDocid && a.m_iDocIndex<b.m_iDocIndex ) );
	}
};


void RtAccum_t::CleanupDuplicates ( int iRowSize )
{
	if ( m_iAccumDocs<=1 )
		return;

	assert ( m_iAccumDocs==m_dPerDocHitsCount.GetLength() );
	CSphVector<AccumDocHits_t> dDocHits ( m_dPerDocHitsCount.GetLength() );
	int iStride = DOCINFO_IDSIZE + iRowSize;

	int iHitIndex = 0;
	CSphRowitem * pRow = m_dAccumRows.Begin();
	for ( int i=0; i<m_iAccumDocs; i++, pRow+=iStride )
	{
		AccumDocHits_t & tElem = dDocHits[i];
		tElem.m_uDocid = DOCINFO2ID ( pRow );
		tElem.m_iDocIndex = i;
		tElem.m_iHitIndex = iHitIndex;
		tElem.m_iHitCount = m_dPerDocHitsCount[i];
		iHitIndex += m_dPerDocHitsCount[i];
	}

	dDocHits.Sort ( CmpDocHitIndex_t() );

	SphDocID_t uPrev = 0;
	if ( !dDocHits.FindFirst ( [&] ( const AccumDocHits_t &dDoc ) {
		bool bRes = dDoc.m_uDocid==uPrev;
		uPrev = dDoc.m_uDocid;
		return bRes;
	} ) )
		return;

	// identify duplicates to kill, and store them in dDocHits
	int iDst = 0;
	if ( m_bReplace )
	{
		// replace mode, last value wins, precending values are duplicate
		for ( int iSrc=0; iSrc<dDocHits.GetLength()-1; iSrc++ )
			if ( dDocHits[iSrc].m_uDocid==dDocHits[iSrc+1].m_uDocid ) // if my next value has the same docid, i am dupe
				dDocHits[iDst++] = dDocHits[iSrc];
	} else
	{
		// insert mode, first value wins, subsequent values are duplicates
		for ( int iSrc=1; iSrc<dDocHits.GetLength(); iSrc++ )
			if ( dDocHits[iSrc].m_uDocid==dDocHits[iSrc-1].m_uDocid ) // if my prev value has the same docid, i am a dupe
				dDocHits[iDst++] = dDocHits[iSrc];
	}
	dDocHits.Resize ( iDst );
	assert ( dDocHits.GetLength() );

	// sort by hit index
	dDocHits.Sort ( bind ( &AccumDocHits_t::m_iHitIndex ) );

	// clean up hits of duplicates
	int iSrc;
	for ( int iHit = dDocHits.GetLength()-1; iHit>=0; iHit-- )
	{
		if ( !dDocHits[iHit].m_iHitCount )
			continue;

		int iFrom = dDocHits[iHit].m_iHitIndex;
		int iCount = dDocHits[iHit].m_iHitCount;
		if ( iFrom+iCount<m_dAccum.GetLength() )
		{
			for ( iDst=iFrom, iSrc=iFrom+iCount; iSrc<m_dAccum.GetLength(); iSrc++, iDst++ )
				m_dAccum[iDst] = m_dAccum[iSrc];
		}
		m_dAccum.Resize ( m_dAccum.GetLength()-iCount );
	}

	// sort by docid index
	dDocHits.Sort ( bind ( &AccumDocHits_t::m_iDocIndex ) );

	// clean up docinfos of duplicates
	for ( int iDoc = dDocHits.GetLength()-1; iDoc>=0; iDoc-- )
	{
		iDst = dDocHits[iDoc].m_iDocIndex*iStride;
		iSrc = iDst+iStride;
		while ( iSrc<m_dAccumRows.GetLength() )
		{
			m_dAccumRows[iDst++] = m_dAccumRows[iSrc++];
		}
		m_iAccumDocs--;
		m_dAccumRows.Resize ( m_iAccumDocs*iStride );
	}
}


void RtAccum_t::GrabLastWarning ( CSphString & sWarning )
{
	if ( m_pDictRt && m_pDictRt->GetLastWarning() )
	{
		sWarning = m_pDictRt->GetLastWarning();
		m_pDictRt->ResetWarning();
	}
}


const RtWord_t * RtIndex_t::CopyWord ( RtSegment_t * pDst, RtWordWriter_t & tOutWord,
	const RtSegment_t * pSrc, const RtWord_t * pWord, RtWordReader_t & tInWord,
	const CSphVector<SphDocID_t> * pAccKlist )
{
	RtDocReader_t tInDoc ( pSrc, *pWord );
	RtDocWriter_t tOutDoc ( pDst );

	RtWord_t tNewWord = *pWord;
	tNewWord.m_uDoc = tOutDoc.ZipDocPtr();

	// if flag is there, acc must be there
	// however, NOT vice versa (newly created segments are unaffected by TLS klist)
	assert (!( pSrc->m_bTlsKlist && !pAccKlist ));
#if 0
	// index *must* be holding acc during merge
	assert ( !pAcc || pAcc->m_pIndex==this );
#endif

	// copy docs
	while (true)
	{
		const RtDoc_t * pDoc = tInDoc.UnzipDoc();
		if ( !pDoc )
			break;

		// apply klist
		bool bKill = ( pSrc->GetKlist().BinarySearch ( pDoc->m_uDocID )!=NULL );
		if ( !bKill && pSrc->m_bTlsKlist )
			bKill = ( pAccKlist->BinarySearch ( pDoc->m_uDocID )!=NULL );

		if ( bKill )
		{
			tNewWord.m_uDocs--;
			tNewWord.m_uHits -= pDoc->m_uHits;
			continue;
		}

		// short route, single embedded hit
		if ( pDoc->m_uHits==1 )
		{
			tOutDoc.ZipDoc ( *pDoc );
			continue;
		}

		// long route, copy hits
		RtHitWriter_t tOutHit ( pDst );
		RtHitReader_t tInHit ( pSrc, pDoc );

		RtDoc_t tDoc = *pDoc;
		tDoc.m_uHit = tOutHit.ZipHitPtr();

		// OPTIMIZE? decode+memcpy?
		for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
			tOutHit.ZipHit ( uValue );

		// copy doc
		tOutDoc.ZipDoc ( tDoc );
	}

	// append word to the dictionary
	if ( tNewWord.m_uDocs )
		tOutWord.ZipWord ( tNewWord );

	// move forward
	return tInWord.UnzipWord ();
}


void RtIndex_t::CopyDoc ( RtSegment_t * pSeg, RtDocWriter_t & tOutDoc, RtWord_t * pWord, const RtSegment_t * pSrc, const RtDoc_t * pDoc )
{
	pWord->m_uDocs++;
	pWord->m_uHits += pDoc->m_uHits;

	if ( pDoc->m_uHits==1 )
	{
		tOutDoc.ZipDoc ( *pDoc );
		return;
	}

	RtHitWriter_t tOutHit ( pSeg );
	RtHitReader_t tInHit ( pSrc, pDoc );

	RtDoc_t tDoc = *pDoc;
	tDoc.m_uHit = tOutHit.ZipHitPtr();
	tOutDoc.ZipDoc ( tDoc );

	// OPTIMIZE? decode+memcpy?
	for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
		tOutHit.ZipHit ( uValue );
}


void RtIndex_t::MergeWord ( RtSegment_t * pSeg, const RtSegment_t * pSrc1, const RtWord_t * pWord1,
	const RtSegment_t * pSrc2, const RtWord_t * pWord2, RtWordWriter_t & tOut,
	const CSphVector<SphDocID_t> * pAccKlist )
{
	assert ( ( !m_bKeywordDict && pWord1->m_uWordID==pWord2->m_uWordID )
		|| ( m_bKeywordDict && sphDictCmpStrictly ( (const char *)pWord1->m_sWord+1, *pWord1->m_sWord, (const char *)pWord2->m_sWord+1, *pWord2->m_sWord )==0 ) );

	RtDocWriter_t tOutDoc ( pSeg );

	RtWord_t tWord;
	if ( !m_bKeywordDict )
		tWord.m_uWordID = pWord1->m_uWordID;
	else
		tWord.m_sWord = pWord1->m_sWord;
	tWord.m_uDocs = 0;
	tWord.m_uHits = 0;
	tWord.m_uDoc = tOutDoc.ZipDocPtr();

	RtDocReader_t tIn1 ( pSrc1, *pWord1 );
	RtDocReader_t tIn2 ( pSrc2, *pWord2 );
	const RtDoc_t * pDoc1 = tIn1.UnzipDoc();
	const RtDoc_t * pDoc2 = tIn2.UnzipDoc();

	while ( pDoc1 || pDoc2 )
	{
		if ( pDoc1 && pDoc2 && pDoc1->m_uDocID==pDoc2->m_uDocID )
		{
			// dupe, must (!) be killed in the first segment, might be in both
#if 0
			assert ( pSrc1->m_dKlist.BinarySearch ( pDoc1->m_uDocID )
				|| ( pSrc1->m_bTlsKlist && pAcc && pAcc->m_dAccumKlist.BinarySearch ( pDoc1->m_uDocID ) ) );
#endif
			if ( !pSrc2->GetKlist().BinarySearch ( pDoc2->m_uDocID )
				&& ( !pSrc1->m_bTlsKlist || !pSrc2->m_bTlsKlist || !pAccKlist->BinarySearch ( pDoc2->m_uDocID ) ) )
				CopyDoc ( pSeg, tOutDoc, &tWord, pSrc2, pDoc2 );
			pDoc1 = tIn1.UnzipDoc();
			pDoc2 = tIn2.UnzipDoc();

		} else if ( pDoc1 && ( !pDoc2 || pDoc1->m_uDocID < pDoc2->m_uDocID ) )
		{
			// winner from the first segment
			if ( !pSrc1->GetKlist().BinarySearch ( pDoc1->m_uDocID )
				&& ( !pSrc1->m_bTlsKlist || !pAccKlist->BinarySearch ( pDoc1->m_uDocID ) ) )
				CopyDoc ( pSeg, tOutDoc, &tWord, pSrc1, pDoc1 );
			pDoc1 = tIn1.UnzipDoc();

		} else
		{
			// winner from the second segment
			assert ( pDoc2 && ( !pDoc1 || pDoc2->m_uDocID < pDoc1->m_uDocID ) );
			if ( !pSrc2->GetKlist().BinarySearch ( pDoc2->m_uDocID )
				&& ( !pSrc2->m_bTlsKlist || !pAccKlist->BinarySearch ( pDoc2->m_uDocID ) ) )
				CopyDoc ( pSeg, tOutDoc, &tWord, pSrc2, pDoc2 );
			pDoc2 = tIn2.UnzipDoc();
		}
	}

	if ( tWord.m_uDocs )
		tOut.ZipWord ( tWord );
}


#if PARANOID
static void CheckSegmentRows ( const RtSegment_t * pSeg, int iStride )
{
	const CSphTightVector<CSphRowitem> & dRows = pSeg->m_dRows; // shortcut
	for ( int i=iStride; i<dRows.GetLength(); i+=iStride )
		assert ( DOCINFO2ID ( &dRows[i] ) > DOCINFO2ID ( &dRows[i-iStride] ) );
}
#endif

template < typename DOCID = SphDocID_t >
struct RtRowIterator_T : public ISphNoncopyable
{
protected:
	const CSphRowitem * m_pRow;
	const CSphRowitem * m_pRowMax;
	const DOCID * m_pTlsKlist;
	const DOCID * m_pTlsKlistMax;
	const int m_iStride;

	const SphDocID_t *	m_pKlist;
	const SphDocID_t *	m_pKlistMax;

public:
	explicit RtRowIterator_T ( const RtSegment_t * pSeg, int iStride, bool bWriter, const CSphVector<DOCID> * pAccKlist, const CSphFixedVector<SphDocID_t> & tKill )
		: m_pRow ( pSeg->m_dRows.Begin() )
		, m_pRowMax ( pSeg->m_dRows.Begin() + pSeg->m_dRows.GetLength() )
		, m_pTlsKlist ( NULL )
		, m_pTlsKlistMax ( NULL )
		, m_iStride ( iStride )
		, m_pKlist ( NULL )
		, m_pKlistMax ( NULL )
	{
		if ( tKill.GetLength() )
		{
			m_pKlist = tKill.Begin();
			m_pKlistMax = tKill.Begin() + tKill.GetLength();
		}

		// FIXME? OPTIMIZE? must not scan tls (open txn) in readers; can implement lighter iterator
		// FIXME? OPTIMIZE? maybe we should just rely on the segment order and don't scan tls klist here
		if ( bWriter && pSeg->m_bTlsKlist && pAccKlist && pAccKlist->GetLength() )
		{
			m_pTlsKlist = pAccKlist->Begin();
			m_pTlsKlistMax = m_pTlsKlist + pAccKlist->GetLength();
		}
	}

	const CSphRowitem * GetNextAliveRow ()
	{
		// while there are rows and k-list entries
		while ( m_pRow<m_pRowMax && ( m_pKlist<m_pKlistMax || m_pTlsKlist<m_pTlsKlistMax ) )
		{
			// get next candidate id
			DOCID uID = DOCINFO2ID_T<DOCID>(m_pRow);

			// check if segment k-list kills it
			while ( m_pKlist<m_pKlistMax && *m_pKlist<uID )
				m_pKlist++;

			if ( m_pKlist<m_pKlistMax && *m_pKlist==uID )
			{
				m_pKlist++;
				m_pRow += m_iStride;
				continue;
			}

			// check if txn k-list kills it
			while ( m_pTlsKlist<m_pTlsKlistMax && *m_pTlsKlist<uID )
				m_pTlsKlist++;

			if ( m_pTlsKlist<m_pTlsKlistMax && *m_pTlsKlist==uID )
			{
				m_pTlsKlist++;
				m_pRow += m_iStride;
				continue;
			}

			// oh, so nobody kills it
			break;
		}

		// oops, out of rows
		if ( m_pRow>=m_pRowMax )
			return NULL;

		// got it, and it's alive!
		m_pRow += m_iStride;
		return m_pRow-m_iStride;
	}
};

typedef RtRowIterator_T<> RtRowIterator_t;

#ifdef PARANOID // sanity check in PARANOID mode
void VerifyEmptyStrings ( const CSphTightVector<BYTE> & dStorage, const CSphSchema & tSchema, const CSphRowitem * pRow )
{
	if ( dStorage.GetLength()>1 )
		return;

	const DWORD * pAttr = DOCINFO2ATTRS(pRow);
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tCol = tSchema.GetAttr(i);
		assert ( tCol.m_eAttrType!=SPH_ATTR_STRING
		|| ( tCol.m_eAttrType==SPH_ATTR_STRING && sphGetRowAttr ( pAttr, tCol.m_tLocator )==0 ) );
	}
}
#endif

static DWORD CopyPackedString ( const BYTE * pSrc, CSphTightVector<BYTE> & dDst )
{
	assert ( pSrc );
	assert ( dDst.GetLength()>=1 );
	const BYTE * pStr = NULL;
	const int iLen = sphUnpackStr ( pSrc, &pStr );
	assert ( iLen>0 );
	assert ( pStr );

	const DWORD uOff = dDst.GetLength();
	const DWORD uWriteLen = iLen + ( pStr - pSrc ); // actual length = strings content length + packed length of string
	dDst.Resize ( uOff + uWriteLen );
	memcpy ( dDst.Begin() + uOff, pSrc, uWriteLen );
	return uOff;
}

static DWORD CopyMva ( const DWORD * pSrc, CSphTightVector<DWORD> & dDst )
{
	assert ( pSrc );
	assert ( dDst.GetLength()>=1 );

	DWORD uCount = *pSrc;
	// plain and rt indexes have different formats for storing empty MVA values
	// plain stores legal offset in attribute and zero in MVA pool
	// rt stores 0 as offset in attribute and non a single byte in MVA pool
	// we should handle here cases where plain was ATTACHed to rt like this
	if ( !uCount )
		return 0;

	DWORD iLen = dDst.GetLength();
	dDst.Resize ( iLen+uCount+1 );
	memcpy ( dDst.Begin()+iLen, pSrc, ( uCount+1 )*sizeof(DWORD) );
	return iLen;
}

static void ExtractLocators ( const CSphSchema & tSchema, ESphAttr eAttrType, CSphVector<CSphAttrLocator> & dLocators )
{
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tColumn = tSchema.GetAttr(i);
		if ( tColumn.m_eAttrType==eAttrType )
			dLocators.Add ( tColumn.m_tLocator );
	}
}


class StorageStringWriter_t : ISphNoncopyable
{
private:
	CSphWriter &					m_tDst;
	CSphVector<CSphAttrLocator>		m_dLocators;

public:
	explicit StorageStringWriter_t ( const CSphSchema & tSchema, CSphWriter & tDst )
		: m_tDst ( tDst )
	{
		ExtractLocators ( tSchema, SPH_ATTR_STRING, m_dLocators );
		ExtractLocators ( tSchema, SPH_ATTR_JSON, m_dLocators );
	}
	const CSphVector<CSphAttrLocator> & GetLocators () const { return m_dLocators; }
	void SetDocid ( SphDocID_t ) {}

	DWORD CopyAttr ( const BYTE * pSrc )
	{
		assert ( m_tDst.GetPos()>0 && m_tDst.GetPos()<( I64C(1)<<32 ) ); // should be 32 bit offset

		const BYTE * pStr = NULL;
		const int iLen = sphUnpackStr ( pSrc, &pStr );
		assert ( iLen && pStr );

		DWORD uAttr = (DWORD)m_tDst.GetPos();
		const int iWriteLen = iLen + ( pStr - pSrc );
		m_tDst.PutBytes ( pSrc, iWriteLen );
		return uAttr;
	}
};


class StorageStringVector_t : ISphNoncopyable
{
private:
	CSphTightVector<BYTE> &			m_dDst;
	CSphVector<CSphAttrLocator>		m_dLocators;

public:
	explicit StorageStringVector_t ( const CSphSchema & tSchema, CSphTightVector<BYTE> & dDst )
		: m_dDst ( dDst )
	{
		ExtractLocators ( tSchema, SPH_ATTR_STRING, m_dLocators );
		ExtractLocators ( tSchema, SPH_ATTR_JSON, m_dLocators );
	}
	const CSphVector<CSphAttrLocator> & GetLocators () const { return m_dLocators; }
	void SetDocid ( SphDocID_t ) {}

	DWORD CopyAttr ( const BYTE * pSrc )
	{
		return CopyPackedString ( pSrc, m_dDst );
	}
};


class StorageMvaWriter_t : ISphNoncopyable
{
private:
	CSphWriter &					m_tDst;
	CSphVector<CSphAttrLocator>		m_dLocators;

public:
	explicit StorageMvaWriter_t ( const CSphSchema & tSchema, CSphWriter & tDst )
		: m_tDst ( tDst )
	{
		ExtractLocators ( tSchema, SPH_ATTR_UINT32SET, m_dLocators );
		ExtractLocators ( tSchema, SPH_ATTR_INT64SET, m_dLocators );
	}
	const CSphVector<CSphAttrLocator> & GetLocators () const { return m_dLocators; }

	void SetDocid ( SphDocID_t uDocid )
	{
		m_tDst.PutDocid ( uDocid );
	}

	DWORD CopyAttr ( const DWORD * pSrc )
	{
		assert ( m_tDst.GetPos()>0 && m_tDst.GetPos()<( I64C(1)<<32 ) ); // should be 32 bit offset

		DWORD uCount = *pSrc;
		assert ( uCount );

		SphOffset_t uOff = m_tDst.GetPos();
		assert ( ( uOff%sizeof(DWORD) )==0 );
		m_tDst.PutBytes ( pSrc, ( uCount+1 )*sizeof(DWORD) );

		return MVA_DOWNSIZE ( uOff/sizeof(DWORD) );
	}
};


class StorageMvaVector_t : ISphNoncopyable
{
private:
	CSphTightVector<DWORD> &		m_dDst;
	CSphVector<CSphAttrLocator>		m_dLocators;

public:
	explicit StorageMvaVector_t ( const CSphSchema & tSchema, CSphTightVector<DWORD> & dDst )
		: m_dDst ( dDst )
	{
		ExtractLocators ( tSchema, SPH_ATTR_UINT32SET, m_dLocators );
		ExtractLocators ( tSchema, SPH_ATTR_INT64SET, m_dLocators );
	}
	const CSphVector<CSphAttrLocator> & GetLocators () const { return m_dLocators; }

	void SetDocid ( SphDocID_t ) {}

	DWORD CopyAttr ( const DWORD * pSrc )
	{
		return CopyMva ( pSrc, m_dDst );
	}
};


template <typename STORAGE, typename SRC>
void CopyFixupStorageAttrs ( const CSphTightVector<SRC> & dSrc, STORAGE & tStorage, CSphRowitem * pRow )
{
	const CSphVector<CSphAttrLocator> & dLocators = tStorage.GetLocators();
	if ( !dLocators.GetLength() )
		return;

	// store string\mva attr for this row
	SphDocID_t uDocid = DOCINFO2ID ( pRow );
	DWORD * pAttr = DOCINFO2ATTRS( pRow );
	bool bIdSet = false;
	ARRAY_FOREACH ( i, dLocators )
	{
		const SphAttr_t uOff = sphGetRowAttr ( pAttr, dLocators[i] );
		if ( !uOff )
			continue;

		assert ( uOff && uOff<dSrc.GetLength() );

		if ( !bIdSet ) // setting docid only on saving MVA to disk for plain index comparability
		{
			tStorage.SetDocid ( uDocid );
			bIdSet = true;
		}

		DWORD uAttr = tStorage.CopyAttr ( dSrc.Begin() + uOff );

		sphSetRowAttr ( pAttr, dLocators[i], uAttr );
	}
}


#define BLOOM_PER_ENTRY_VALS_COUNT 8
#define BLOOM_HASHES_COUNT 2
#define BLOOM_NGRAM_0 2
#define BLOOM_NGRAM_1 4

struct BloomGenTraits_t
{
	uint64_t * m_pBuf = nullptr;
	explicit BloomGenTraits_t ( uint64_t * pBuf )
		: m_pBuf ( pBuf )
	{}

	void Set ( int iPos, uint64_t uVal )
	{
		m_pBuf[iPos] |= uVal;
	}

	bool IterateNext() const { return true; }
};

struct BloomCheckTraits_t
{
	const uint64_t * m_pBuf = nullptr;
	bool m_bSame = true;
	explicit BloomCheckTraits_t ( const uint64_t * pBuf )
		: m_pBuf ( pBuf )
	{}

	void Set ( int iPos, uint64_t uVal )
	{
		m_bSame = ( ( m_pBuf[iPos] & uVal )==uVal );
	}

	bool IterateNext() const { return m_bSame; }
};

template <typename BLOOM_TRAITS = BloomGenTraits_t>
bool BuildBloom ( const BYTE * sWord, int iLen, int iInfixCodepointCount, bool bUtf8, int iKeyValCount, BLOOM_TRAITS & tBloom )
{
	if ( iLen<iInfixCodepointCount )
		return false;
	// byte offset for each codepoints
	BYTE dOffsets [ SPH_MAX_WORD_LEN+1 ] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
		20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42 };
	assert ( iLen<=SPH_MAX_WORD_LEN || ( bUtf8 && iLen<=SPH_MAX_WORD_LEN*3 ) );
	int iCodes = iLen;
	if ( bUtf8 )
	{
		// build an offsets table into the bytestring
		iCodes = 0;
		const BYTE * s = sWord;
		const BYTE * sEnd = sWord + iLen;
		while ( s<sEnd )
		{
			int iCodepoints = sphUtf8CharBytes ( *s );
			assert ( iCodepoints>=1 && iCodepoints<=4 );
			dOffsets[iCodes+1] = dOffsets[iCodes] + (BYTE)iCodepoints;
			s += iCodepoints;
			iCodes++;
		}
	}
	if ( iCodes<iInfixCodepointCount )
		return false;

	int iKeyBytes = iKeyValCount * 64;
	for ( int i=0; i<=iCodes-iInfixCodepointCount && tBloom.IterateNext(); i++ )
	{
		int iFrom = dOffsets[i];
		int iTo = dOffsets[i+iInfixCodepointCount];
		uint64_t uHash64 = sphFNV64 ( sWord+iFrom, iTo-iFrom );

		uHash64 = ( uHash64>>32 ) ^ ( (DWORD)uHash64 );
		int iByte = (int)( uHash64 % iKeyBytes );
		int iPos = iByte/64;
		uint64_t uVal = U64C(1) << ( iByte % 64 );

		tBloom.Set ( iPos, uVal );
	}
	return true;
}


static void BuildSegmentInfixes ( RtSegment_t * pSeg, bool bHasMorphology, bool bKeywordDict, int iMinInfixLen, int iWordsCheckpoint, bool bUtf8 )
{
	if ( !pSeg || !bKeywordDict || !iMinInfixLen )
		return;

	int iBloomSize = ( pSeg->m_dWordCheckpoints.GetLength()+1 ) * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
	pSeg->m_dInfixFilterCP.Resize ( iBloomSize );
	// reset filters
	pSeg->m_dInfixFilterCP.Fill ( 0 );

	uint64_t * pRough = pSeg->m_dInfixFilterCP.Begin();
	const RtWord_t * pWord = nullptr;
	RtWordReader_t rdDictRough ( pSeg, true, iWordsCheckpoint );
	while ( ( pWord = rdDictRough.UnzipWord () )!=nullptr )
	{
		const BYTE * pDictWord = pWord->m_sWord+1;
		if ( bHasMorphology && *pDictWord!=MAGIC_WORD_HEAD_NONSTEMMED )
			continue;

		int iLen = pWord->m_sWord[0];
		if ( *pDictWord<0x20 ) // anyway skip heading magic chars in the prefix, like NONSTEMMED maker
		{
			pDictWord++;
			iLen--;
		}

		uint64_t * pVal = pRough + rdDictRough.m_iCheckpoint * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
		BloomGenTraits_t tBloom0 ( pVal );
		BloomGenTraits_t tBloom1 ( pVal+BLOOM_PER_ENTRY_VALS_COUNT );
		BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_0, bUtf8, BLOOM_PER_ENTRY_VALS_COUNT, tBloom0 );
		BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_1, bUtf8, BLOOM_PER_ENTRY_VALS_COUNT, tBloom1 );
	}
}


RtSegment_t * RtIndex_t::MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2, const CSphVector<SphDocID_t> * pAccKlist, bool bHasMorphology )
{
	if ( pSeg1->m_iTag > pSeg2->m_iTag )
		Swap ( pSeg1, pSeg2 );

	auto * pSeg = new RtSegment_t ();

	////////////////////
	// merge attributes
	////////////////////

	// check that all the IDs are in proper asc order
#if PARANOID
	CheckSegmentRows ( pSeg1, m_iStride );
	CheckSegmentRows ( pSeg2, m_iStride );
#endif

	// just a shortcut
	CSphTightVector<CSphRowitem> & dRows = pSeg->m_dRows;
	CSphTightVector<BYTE> & dStrings = pSeg->m_dStrings;
	CSphTightVector<DWORD> & dMvas = pSeg->m_dMvas;

	// we might need less because of dupes, but we can not know yet
	dRows.Reserve ( Max ( pSeg1->m_dRows.GetLength(), pSeg2->m_dRows.GetLength() ) );

	// as each segment has dummy zero we reserve less
	assert ( pSeg1->m_dStrings.GetLength() + pSeg2->m_dStrings.GetLength()>=2 );
	dStrings.Reserve ( Max ( pSeg1->m_dStrings.GetLength(), pSeg2->m_dStrings.GetLength() ) );
	assert ( pSeg1->m_dMvas.GetLength() + pSeg2->m_dMvas.GetLength()>=2 );
	dMvas.Reserve ( Max ( pSeg1->m_dMvas.GetLength(), pSeg2->m_dMvas.GetLength() ) );

	StorageStringVector_t tStorageString ( m_tSchema, dStrings );
	StorageMvaVector_t tStorageMva ( m_tSchema, dMvas );

	RtRowIterator_t tIt1 ( pSeg1, m_iStride, true, pAccKlist, pSeg1->GetKlist() );
	RtRowIterator_t tIt2 ( pSeg2, m_iStride, true, pAccKlist, pSeg2->GetKlist() );

	const CSphRowitem * pRow1 = tIt1.GetNextAliveRow();
	const CSphRowitem * pRow2 = tIt2.GetNextAliveRow();

	while ( pRow1 || pRow2 )
	{
		if ( !pRow2 || ( pRow1 && pRow2 && DOCINFO2ID(pRow1)<DOCINFO2ID(pRow2) ) )
		{
			assert ( pRow1 );
			for ( int i=0; i<m_iStride; ++i )
				dRows.Add ( *pRow1++ );
			CSphRowitem * pDstRow = dRows.Begin() + dRows.GetLength() - m_iStride;
			CopyFixupStorageAttrs ( pSeg1->m_dStrings, tStorageString, pDstRow );
			CopyFixupStorageAttrs ( pSeg1->m_dMvas, tStorageMva, pDstRow );
			pRow1 = tIt1.GetNextAliveRow();
		} else
		{
			assert ( pRow2 );
			assert ( !pRow1 || ( DOCINFO2ID(pRow1)!=DOCINFO2ID(pRow2) ) ); // all dupes must be killed and skipped by the iterator
			for ( int i=0; i<m_iStride; ++i )
				dRows.Add ( *pRow2++ );
			CSphRowitem * pDstRow = dRows.Begin() + dRows.GetLength() - m_iStride;
			CopyFixupStorageAttrs ( pSeg2->m_dStrings, tStorageString, pDstRow );
			CopyFixupStorageAttrs ( pSeg2->m_dMvas, tStorageMva, pDstRow );
			pRow2 = tIt2.GetNextAliveRow();
		}
		++pSeg->m_iRows;
		++pSeg->m_iAliveRows;
	}

	assert ( pSeg->m_iRows*m_iStride==pSeg->m_dRows.GetLength() );
#if PARANOID
	CheckSegmentRows ( pSeg, m_iStride );
#endif

	// merged segment might be completely killed by committed data
	if ( !pSeg->m_iRows )
	{
		SafeDelete ( pSeg );
		return NULL;
	}

	//////////////////
	// merge keywords
	//////////////////

	pSeg->m_dWords.Reserve ( Max ( pSeg1->m_dWords.GetLength(), pSeg2->m_dWords.GetLength() ) );
	pSeg->m_dDocs.Reserve ( Max ( pSeg1->m_dDocs.GetLength(), pSeg2->m_dDocs.GetLength() ) );
	pSeg->m_dHits.Reserve ( Max ( pSeg1->m_dHits.GetLength(), pSeg2->m_dHits.GetLength() ) );

	RtWordWriter_t tOut ( pSeg, m_bKeywordDict, m_iWordsCheckpoint );
	RtWordReader_t tIn1 ( pSeg1, m_bKeywordDict, m_iWordsCheckpoint );
	RtWordReader_t tIn2 ( pSeg2, m_bKeywordDict, m_iWordsCheckpoint );
	const RtWord_t * pWords1 = tIn1.UnzipWord ();
	const RtWord_t * pWords2 = tIn2.UnzipWord ();

	// merge while there are common words
	while (true)
	{
		while ( pWords1 && pWords2 )
		{
			int iCmp = 0;
			if ( m_bKeywordDict )
			{
				iCmp = sphDictCmpStrictly ( (const char *)pWords1->m_sWord+1, *pWords1->m_sWord, (const char *)pWords2->m_sWord+1, *pWords2->m_sWord );
			} else
			{
				if ( pWords1->m_uWordID<pWords2->m_uWordID )
					iCmp = -1;
				else if ( pWords1->m_uWordID>pWords2->m_uWordID )
					iCmp = 1;
			}

			if ( !iCmp )
				break;

			if ( iCmp<0 )
				pWords1 = CopyWord ( pSeg, tOut, pSeg1, pWords1, tIn1, pAccKlist );
			else
				pWords2 = CopyWord ( pSeg, tOut, pSeg2, pWords2, tIn2, pAccKlist );
		}

		if ( !pWords1 || !pWords2 )
			break;

		assert ( pWords1 && pWords2 &&
			( ( !m_bKeywordDict && pWords1->m_uWordID==pWords2->m_uWordID )
			|| ( m_bKeywordDict && sphDictCmpStrictly ( (const char *)pWords1->m_sWord+1, *pWords1->m_sWord, (const char *)pWords2->m_sWord+1, *pWords2->m_sWord )==0 ) ) );
		MergeWord ( pSeg, pSeg1, pWords1, pSeg2, pWords2, tOut, pAccKlist );
		pWords1 = tIn1.UnzipWord();
		pWords2 = tIn2.UnzipWord();
	}

	// copy tails
	while ( pWords1 ) pWords1 = CopyWord ( pSeg, tOut, pSeg1, pWords1, tIn1, pAccKlist );
	while ( pWords2 ) pWords2 = CopyWord ( pSeg, tOut, pSeg2, pWords2, tIn2, pAccKlist );

	if ( m_bKeywordDict )
		FixupSegmentCheckpoints ( pSeg );

	BuildSegmentInfixes ( pSeg, bHasMorphology, m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ) );

	assert ( pSeg->m_dRows.GetLength() );
	assert ( pSeg->m_iRows );
	assert ( pSeg->m_iAliveRows==pSeg->m_iRows );
	return pSeg;
}


struct CmpSegments_fn
{
	inline bool IsLess ( const RtSegment_t * a, const RtSegment_t * b ) const
	{
			return a->GetMergeFactor() > b->GetMergeFactor();
	}
};


void RtIndex_t::Commit ( int * pDeleted, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	MEMORY ( MEM_INDEX_RT );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict );
	if ( !pAcc )
		return;

	// empty txn, just ignore
	if ( !pAcc->m_iAccumDocs && !pAcc->m_dAccumKlist.GetLength() )
	{
		pAcc->SetIndex ( nullptr );
		pAcc->Cleanup ( RtAccum_t::EPartial );
		return;
	}

	// phase 0, build a new segment
	// accum and segment are thread local; so no locking needed yet
	// segment might be NULL if we're only killing rows this txn
	pAcc->CleanupDuplicates ( m_tSchema.GetRowSize() );
	pAcc->Sort();

	RtSegment_t * pNewSeg = pAcc->CreateSegment ( m_tSchema.GetRowSize(), m_iWordsCheckpoint );
	assert ( !pNewSeg || pNewSeg->m_iRows>0 );
	assert ( !pNewSeg || pNewSeg->m_iAliveRows>0 );
	assert ( !pNewSeg || pNewSeg->m_bTlsKlist==false );

	BuildSegmentInfixes ( pNewSeg, m_pDict->HasMorphology(), m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ) );

#if PARANOID
	if ( pNewSeg )
		CheckSegmentRows ( pNewSeg, m_iStride );
#endif

	// clean up parts we no longer need
	pAcc->Cleanup ( RtAccum_t::EPartial | RtAccum_t::EAccum );

	// sort accum klist, too
	pAcc->m_dAccumKlist.Uniq ();

	// now on to the stuff that needs locking and recovery
	CommitReplayable ( pNewSeg, pAcc->m_dAccumKlist, pDeleted );

	// done; cleanup accum
	pAcc->Cleanup ( RtAccum_t::ERest );
	// reset accumulated warnings
	CSphString sWarning;
	pAcc->GrabLastWarning ( sWarning );
}

void RtIndex_t::CommitReplayable ( RtSegment_t * pNewSeg, CSphVector<SphDocID_t> & dAccKlist, int * pTotalKilled )
{
	// store statistics, because pNewSeg just might get merged
	int iNewDocs = pNewSeg ? pNewSeg->m_iRows : 0;

	CSphVector<int64_t> dLens;
	int iFirstFieldLenAttr = m_tSchema.GetAttrId_FirstFieldLen();
	if ( pNewSeg && iFirstFieldLenAttr>=0 )
	{
		assert ( pNewSeg->GetStride()==m_iStride );
		int iFields = m_tSchema.GetFieldsCount(); // shortcut
		dLens.Resize ( iFields );
		dLens.Fill ( 0 );
		for ( int i=0; i<pNewSeg->m_iRows; ++i )
			for ( int j=0; j<iFields; ++j )
				dLens[j] += sphGetRowAttr ( &pNewSeg->m_dRows [ i*m_iStride+DOCINFO_IDSIZE ], m_tSchema.GetAttr ( j+iFirstFieldLenAttr ).m_tLocator );
	}

	// phase 1, lock out other writers (but not readers yet)
	// concurrent readers are ok during merges, as existing segments won't be modified yet
	// however, concurrent writers are not
	Verify ( m_tWriting.Lock() );

	// first of all, binlog txn data for recovery
	g_pRtBinlog->BinlogCommit ( &m_iTID, m_sIndexName.cstr(), pNewSeg, dAccKlist, m_bKeywordDict );
	int64_t iTID = m_iTID;

	// let merger know that existing segments are subject to additional, TLS K-list filter
	// safe despite the readers, flag must only be used by writer
	if ( dAccKlist.GetLength() )
		for ( int i=m_iDoubleBuffer; i<m_dRamChunks.GetLength(); ++i )
		{
			// OPTIMIZE? only need to set the flag if TLS K-list *actually* affects segment
			assert ( m_dRamChunks[i]->m_bTlsKlist==false );
			m_dRamChunks[i]->m_bTlsKlist = true;
		}

	// prepare new segments vector
	// create more new segments by merging as needed
	// do not (!) kill processed old segments just yet, as readers might still need them
	CSphVector<RtSegment_t*> dSegments;

	dSegments.Reserve ( m_dRamChunks.GetLength() - m_iDoubleBuffer + 1 );
	for ( int i=m_iDoubleBuffer; i<m_dRamChunks.GetLength(); ++i )
		dSegments.Add ( m_dRamChunks[i] );
	if ( pNewSeg )
		dSegments.Add ( pNewSeg );

	int64_t iRamFreed = 0;
	bool bHasMorphology = m_pDict->HasMorphology();
	FreeRetired();

	// enforce RAM usage limit
	int64_t iRamLeft = m_iDoubleBuffer ? m_iDoubleBufferLimit : m_iSoftRamLimit;
	for ( const auto& dSegment : dSegments )
		iRamLeft = Max ( iRamLeft - dSegment->GetUsedRam(), 0 );
	for ( const auto& dRetired : m_dRetired )
		iRamLeft = Max ( iRamLeft - dRetired->GetUsedRam(), 0 );

	// skip merging if no rows were added or no memory left
	bool bDump = ( iRamLeft==0 );
	const int MAX_SEGMENTS = 32;
	const int MAX_PROGRESSION_SEGMENT = 8;
	const int64_t MAX_SEGMENT_VECTOR_LEN = INT_MAX;
	while ( pNewSeg && iRamLeft>0 )
	{
		// segments sort order: large first, smallest last
		// merge last smallest segments
		dSegments.Sort ( CmpSegments_fn() );

		// unconditionally merge if there's too much segments now
		// conditionally merge if smallest segment has grown too large
		// otherwise, we're done
		const int iLen = dSegments.GetLength();
		if ( iLen < ( MAX_SEGMENTS - MAX_PROGRESSION_SEGMENT ) )
			break;
		assert ( iLen>=2 );
		// exit if progression is kept AND lesser MAX_SEGMENTS limit
		if ( dSegments[iLen-2]->GetMergeFactor() > dSegments[iLen-1]->GetMergeFactor()*2 && iLen < MAX_SEGMENTS )
			break;

		// check whether we have enough RAM
#define LOC_ESTIMATE1(_seg,_vec) \
	(int)( ( (int64_t)_seg->_vec.GetLength() ) * _seg->m_iAliveRows / _seg->m_iRows )

#define LOC_ESTIMATE(_vec) \
	( LOC_ESTIMATE1 ( dSegments[iLen-1], _vec ) + LOC_ESTIMATE1 ( dSegments[iLen-2], _vec ) )

		using namespace sph;
		int64_t iWordsRelimit = TightRelimit::Relimit ( 0, LOC_ESTIMATE ( m_dWords ) );
		int64_t iDocsRelimit = TightRelimit::Relimit ( 0, LOC_ESTIMATE ( m_dDocs ) );
		int64_t iHitsRelimit = TightRelimit::Relimit ( 0, LOC_ESTIMATE ( m_dHits ) );
		int64_t iStringsRelimit = TightRelimit::Relimit ( 0, LOC_ESTIMATE ( m_dStrings ) );
		int64_t iMvasRelimit = TightRelimit::Relimit ( 0, LOC_ESTIMATE ( m_dMvas ) );
		int64_t iKeywordsRelimit = TightRelimit::Relimit ( 0, LOC_ESTIMATE ( m_dKeywordCheckpoints ) );
		int64_t iRowsRelimit = TightRelimit::Relimit ( 0, LOC_ESTIMATE ( m_dRows ) );

#undef LOC_ESTIMATE
#undef LOC_ESTIMATE1

		int64_t iEstimate = iWordsRelimit + iDocsRelimit + iHitsRelimit + iStringsRelimit + iMvasRelimit + iKeywordsRelimit + iRowsRelimit;
		if ( iEstimate>iRamLeft )
		{
			// dump case: can't merge any more AND segments count limit's reached
			bDump = ( ( iRamLeft + iRamFreed )<=iEstimate ) && ( iLen>=MAX_SEGMENTS );
			break;
		}

		// we have to dump if we can't merge even smallest segments without breaking vector constrain ( len<INT_MAX )
		// split this way to avoid superlong string after macro expansion that kills gcov
		int64_t iMaxLen = Max (
			Max ( iWordsRelimit, iDocsRelimit ),
			Max ( iHitsRelimit, iStringsRelimit ) );
		iMaxLen = Max (
			Max ( iMvasRelimit, iKeywordsRelimit ),
			Max ( iMaxLen, iRowsRelimit ) );

		if ( MAX_SEGMENT_VECTOR_LEN<iMaxLen )
		{
			bDump = true;
			break;
		}

		// do it
		RtSegment_t * pA = dSegments.Pop();
		RtSegment_t * pB = dSegments.Pop();
		RtSegment_t * pMerged = MergeSegments ( pA, pB, &dAccKlist, bHasMorphology );
		if ( pMerged )
		{
			int64_t iMerged = pMerged->GetUsedRam();
			iRamLeft -= Min ( iRamLeft, iMerged );
			dSegments.Add ( pMerged );
		}
		m_dRetired.Add ( pA );
		m_dRetired.Add ( pB );

		iRamFreed += pA->GetUsedRam() + pB->GetUsedRam();
	}

	// phase 2, obtain exclusive writer lock
	// we now have to update K-lists in (some of) the survived segments
	// and also swap in new segment list

	// adjust for an incoming accumulator K-list
	int iTotalKilled = 0;
	int iDiskLiveKLen = 0;
	if ( dAccKlist.GetLength() )
	{
		// update totals
		// work the original (!) segments, and before (!) updating their K-lists
		iDiskLiveKLen = dAccKlist.GetLength();
		for ( int i=0; i<iDiskLiveKLen; i++ )
		{
			SphDocID_t uDocid = dAccKlist[i];

			// the most recent part of RT index is its RAM chunk, so first search it
			// then search chunk which is saving right now
			// after that search disk chunks in order from younger to older ones
			// if doc is killed in younger index part then it's really killed - no need to search older parts
			bool bRamAlive = false;
			bool bSavedOrDiskAlive = false;
			bool bAlreadyKilled = false;
			while (true)
			{
				for ( int j=m_dRamChunks.GetLength()-1; j>=m_iDoubleBuffer && !bRamAlive; --j )
					bRamAlive = !!m_dRamChunks[j]->FindAliveRow ( uDocid );
				if ( bRamAlive )
					break;

				// killed in saved or one of disk chunks? but not during double buffer work
				if ( !m_iDoubleBuffer && m_tKlist.Exists ( uDocid ) )
				{
					bAlreadyKilled = true;
					break;
				}

				for ( int j=m_iDoubleBuffer-1; j>=0 && !bSavedOrDiskAlive; --j )
					bSavedOrDiskAlive = !!m_dRamChunks[j]->FindAliveRow ( uDocid );
				if ( bSavedOrDiskAlive )
					break;

				// killed in one of disk chunks?
				if ( m_dDiskChunkKlist.BinarySearch ( uDocid ) )
					break;

				for ( int j=m_dDiskChunks.GetLength()-1; j>=0; --j )
				{
					bSavedOrDiskAlive = m_dDiskChunks[j]->HasDocid ( uDocid );
					if ( bSavedOrDiskAlive )
						break;
					// killed in previous disk chunks?
					if ( sphBinarySearch ( m_dDiskChunks[j]->GetKillList(), m_dDiskChunks[j]->GetKillList()+m_dDiskChunks[j]->GetKillListSize()-1, uDocid ) )
						break;
				}

				break;
			}

			if ( bRamAlive || bSavedOrDiskAlive )
				++iTotalKilled;

			if ( bAlreadyKilled || !bSavedOrDiskAlive )
			{
				// we can't just RemoveFast() elements from vector
				// because we'll use its values with indexes >=iDiskLiveKLen in segments kill lists just below
				Swap ( dAccKlist[i], dAccKlist[iDiskLiveKLen-1] );
				--iDiskLiveKLen;
				--i;
			}
		}

		CSphVector<SphDocID_t> dSegmentKlist;

		// update K-lists on survivors
		ARRAY_FOREACH ( iSeg, dSegments )
		{
			RtSegment_t * pSeg = dSegments[iSeg];
			if ( !pSeg->m_bTlsKlist )
				continue; // should be fresh enough

			dSegmentKlist.Resize ( 0 );
			for ( SphDocID_t uDocid : dAccKlist )
				if ( pSeg->FindAliveRow ( uDocid ) )
					dSegmentKlist.Add ( uDocid );

			// now actually update it
			if ( dSegmentKlist.GetLength() )
			{
				int iAdded = dSegmentKlist.GetLength ();
				dSegmentKlist.Append ( pSeg->GetKlist ().begin(), pSeg->GetKlist ().GetLength () );
				dSegmentKlist.Uniq();

				auto * pKlist = new KlistRefcounted_t();
				pKlist->m_dKilled.CopyFrom ( dSegmentKlist );

				// swap data, update counters
				m_tChunkLock.WriteLock();

				Swap ( pSeg->m_pKlist, pKlist ); // hold swapped kill-list for postponed delete
				pSeg->m_iAliveRows -= iAdded;
				assert ( pSeg->m_iAliveRows>=0 );

				m_tChunkLock.Unlock();
				SafeRelease ( pKlist );
			}

			// mark as good
			pSeg->m_bTlsKlist = false;
		}

		// collect kill-list for new segments
		if ( m_iDoubleBuffer )
		{
			int iOff = m_dNewSegmentKlist.GetLength();
			m_dNewSegmentKlist.Resize ( iOff + iDiskLiveKLen );
			memcpy ( m_dNewSegmentKlist.Begin()+iOff, dAccKlist.Begin(), sizeof(SphDocID_t)*iDiskLiveKLen );
		}
	}

	// update saved chunk and disk chunks kill list
	// after iDiskLiveKLen IDs are already killed or don't exist - just skip them
	if ( iDiskLiveKLen )
		m_tKlist.Add ( dAccKlist.Begin(), iDiskLiveKLen );

	ARRAY_FOREACH ( i, dSegments )
	{
		RtSegment_t * pSeg = dSegments[i];
		if ( pSeg->m_iAliveRows==0 )
		{
			m_dRetired.Add ( pSeg );
			dSegments.RemoveFast ( i );
			--i;
		}
	}

	// wipe out readers - now we are only using RAM segments
	m_tChunkLock.WriteLock ();

	// go live!
	// got rid of 'old' double-buffer segments then add 'new' onces
	m_dRamChunks.Resize ( m_iDoubleBuffer + dSegments.GetLength() );
	memcpy ( m_dRamChunks.Begin() + m_iDoubleBuffer, dSegments.Begin(), dSegments.GetLengthBytes() );

	// phase 3, enable readers again
	// we might need to dump data to disk now
	// but during the dump, readers can still use RAM chunk data
	Verify ( m_tChunkLock.Unlock() );

	// update stats
	m_tStats.m_iTotalDocuments += iNewDocs - iTotalKilled;

	if ( dLens.GetLength() )
		for ( int i = 0; i < m_tSchema.GetFieldsCount(); i++ )
		{
			m_dFieldLensRam[i] += dLens[i];
			m_dFieldLens[i] = m_dFieldLensRam[i] + m_dFieldLensDisk[i];
		}

	// get flag of double-buffer prior mutex unlock
	bool bDoubleBufferActive = ( m_iDoubleBuffer>0 );

	// tell about DELETE affected_rows
	if ( pTotalKilled )
		*pTotalKilled = iTotalKilled;

	// we can kill retired segments now
	FreeRetired();

	// double buffer writer stands still till save done
	// all writers waiting double buffer done
	// no need to dump or waiting for some writer
	if ( !bDump || bDoubleBufferActive )
	{
		// all done, enable other writers
		Verify ( m_tWriting.Unlock() );
		return;
	}

	// scope for guard then retired clean up
	{
		// copy stats for disk chunk
		SphChunkGuard_t tGuard;
		GetReaderChunks ( tGuard );

		ChunkStats_t tStat2Dump ( m_tStats, m_dFieldLensRam );
		m_iDoubleBuffer = m_dRamChunks.GetLength();

		m_dDiskChunkKlist.Resize ( 0 );
		m_tKlist.Flush ( m_dDiskChunkKlist );

		// need release m_tReading lock to prevent deadlock - commit vs SaveDiskChunk
		// chunks will keep till this scope and
		// will be freed after this scope on next commit at FreeRetired under writer lock
		ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
			m_dRetired.Add ( tGuard.m_dRamChunks[i] );
		tGuard.m_pReading->Unlock();
		tGuard.m_pReading = nullptr;

		Verify ( m_tWriting.Unlock() );

		SaveDiskChunk ( iTID, tGuard, tStat2Dump, false );
		g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), iTID, false );
	}

	// TODO - try to call FreeRetired after take writer lock again
	// to free more memory
}


void RtIndex_t::FreeRetired()
{
	m_dRetired.Uniq();
	ARRAY_FOREACH ( i, m_dRetired )
	{
		if ( m_dRetired[i]->m_tRefCount.GetValue()==0 )
		{
			SafeDelete ( m_dRetired[i] );
			m_dRetired.RemoveFast ( i );
			i--;
		}
	}
}


void RtIndex_t::RollBack ( ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict );
	if ( !pAcc )
		return;

	pAcc->Cleanup ();
}

bool RtIndex_t::DeleteDocument ( const SphDocID_t * pDocs, int iDocs, CSphString & sError, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	MEMORY ( MEM_RT_ACCUM );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict, true, &sError );
	if ( !pAcc )
		return false;

	if ( !iDocs )
		return true;

	assert ( pDocs && iDocs );

	// !COMMIT should handle case when uDoc what inserted in current txn here
	while ( iDocs-- )
		pAcc->m_dAccumKlist.Add ( *pDocs++ );

	return true;
}

//////////////////////////////////////////////////////////////////////////
// LOAD/SAVE
//////////////////////////////////////////////////////////////////////////

struct Checkpoint_t
{
	uint64_t m_uWord;
	uint64_t m_uOffset;
};


void RtIndex_t::ForceDiskChunk () NO_THREAD_SAFETY_ANALYSIS
{
	MEMORY ( MEM_INDEX_RT );

	if ( !m_dRamChunks.GetLength() )
		return;

	Verify ( m_tWriting.Lock() );

	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	m_dDiskChunkKlist.Resize ( 0 );
	m_tKlist.Flush ( m_dDiskChunkKlist );
	Verify ( m_tWriting.Unlock() );

	ChunkStats_t tStats ( m_tStats, m_dFieldLensRam );
	SaveDiskChunk ( m_iTID, tGuard, tStats, true );
}


struct SaveSegment_t
{
	const RtSegment_t *		m_pSeg;
	const CSphFixedVector<SphDocID_t> * m_pKill;
};


void RtIndex_t::SaveDiskDataImpl ( const char * sFilename, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats ) const
{
	typedef RtDoc_T<SphDocID_t> RTDOC;
	typedef RtWord_T<SphWordID_t> RTWORD;

	CSphString sName, sError; // FIXME!!! report collected (sError) errors

	CSphWriter wrHits, wrDocs, wrDict, wrRows, wrSkips;
	sName.SetSprintf ( "%s.spp", sFilename ); wrHits.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spd", sFilename ); wrDocs.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spi", sFilename ); wrDict.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spa", sFilename ); wrRows.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spe", sFilename ); wrSkips.OpenFile ( sName.cstr(), sError );


	wrDict.PutByte ( 1 );
	wrDocs.PutByte ( 1 );
	wrHits.PutByte ( 1 );
	wrSkips.PutByte ( 1 );

	// we don't have enough RAM to create new merged segments
	// and have to do N-way merge kinda in-place
	CSphVector<RtWordReader_T<SphWordID_t>*> pWordReaders;
	CSphVector<RtDocReader_T<SphDocID_t>*> pDocReaders;
	CSphVector<SaveSegment_t> pSegments;
	CSphVector<const RTWORD*> pWords;
	CSphVector<const RTDOC*> pDocs;

	int iSegments = tGuard.m_dRamChunks.GetLength();

	pWordReaders.Reserve ( iSegments );
	pDocReaders.Reserve ( iSegments );
	pSegments.Reserve ( iSegments );
	pWords.Reserve ( iSegments );
	pDocs.Reserve ( iSegments );

	////////////////////
	// write attributes
	////////////////////

	// the new, template-param aligned iStride instead of index-wide
	int iStride = DWSIZEOF(SphDocID_t) + m_tSchema.GetRowSize();
	CSphFixedVector<RtRowIterator_T<SphDocID_t>*> pRowIterators ( iSegments );
	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
		pRowIterators[i] = new RtRowIterator_T<SphDocID_t> ( tGuard.m_dRamChunks[i], iStride, false, NULL, tGuard.m_dKill[i]->m_dKilled );

	CSphVector<const CSphRowitem*> pRows ( iSegments );
	ARRAY_FOREACH ( i, pRowIterators )
		pRows[i] = pRowIterators[i]->GetNextAliveRow();

	// prepare to build min-max index for attributes too
	int iTotalDocs = 0;
	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
		iTotalDocs += tGuard.m_dRamChunks[i]->m_iAliveRows;

	AttrIndexBuilder_t<SphDocID_t> tMinMaxBuilder ( m_tSchema );
	CSphVector<DWORD> dMinMaxBuffer ( int ( tMinMaxBuilder.GetExpectedSize ( iTotalDocs ) ) ); // RT index doesn't support over 4Gb .spa
	tMinMaxBuilder.Prepare ( dMinMaxBuffer.Begin(), dMinMaxBuffer.Begin() + dMinMaxBuffer.GetLength() );

	sName.SetSprintf ( "%s.sps", sFilename );
	CSphWriter tStrWriter;
	tStrWriter.OpenFile ( sName.cstr(), sError );
	tStrWriter.PutByte ( 0 ); // dummy byte, to reserve magic zero offset

	sName.SetSprintf ( "%s.spm", sFilename );
	CSphWriter tMvaWriter;
	tMvaWriter.OpenFile ( sName.cstr(), sError );
	tMvaWriter.PutDword ( 0 ); // dummy dword, to reserve magic zero offset

	SphDocID_t iMinDocID = DOCID_MAX;
	CSphRowitem * pFixedRow = new CSphRowitem[iStride];

#ifndef NDEBUG
	int iStoredDocs = 0;
#endif

	StorageStringWriter_t tStorageString ( m_tSchema, tStrWriter );
	StorageMvaWriter_t tStorageMva ( m_tSchema, tMvaWriter );

	while (true)
	{
		// find min row
		int iMinRow = -1;
		ARRAY_FOREACH ( i, pRows )
			if ( pRows[i] )
				if ( iMinRow<0 || DOCINFO2ID ( pRows[i] ) < DOCINFO2ID ( pRows[iMinRow] ) )
					iMinRow = i;
		if ( iMinRow<0 )
			break;

#ifndef NDEBUG
		// verify that it's unique
		int iDupes = 0;
		ARRAY_FOREACH ( i, pRows )
			if ( pRows[i] )
				if ( DOCINFO2ID ( pRows[i] )==DOCINFO2ID ( pRows[iMinRow] ) )
					iDupes++;
		assert ( iDupes==1 );
#endif

		const CSphRowitem * pRow = pRows[iMinRow];

		// strings storage for stored row
		assert ( iMinRow<iSegments );
		const RtSegment_t * pSegment = tGuard.m_dRamChunks[iMinRow];

#ifdef PARANOID // sanity check in PARANOID mode
		VerifyEmptyStrings ( pSegment->m_dStrings, m_tSchema, pRow );
#endif

		// collect min-max data
		Verify ( tMinMaxBuilder.Collect ( pRow, pSegment->m_dMvas.Begin(), pSegment->m_dMvas.GetLength(), sError, false ) );

		if ( iMinDocID==DOCID_MAX )
			iMinDocID = DOCINFO2ID ( pRows[iMinRow] );

		if ( pSegment->m_dStrings.GetLength()>1 || pSegment->m_dMvas.GetLength()>1 ) // should be more then dummy zero elements
		{
			// copy row content as we'll fix up its attrs ( string offset for now )
			memcpy ( pFixedRow, pRow, iStride*sizeof(CSphRowitem) );
			pRow = pFixedRow;

			CopyFixupStorageAttrs ( pSegment->m_dStrings, tStorageString, pFixedRow );
			CopyFixupStorageAttrs ( pSegment->m_dMvas, tStorageMva, pFixedRow );
		}

		// emit it
		wrRows.PutBytes ( pRow, iStride*sizeof(CSphRowitem) );

		// fast forward
		pRows[iMinRow] = pRowIterators[iMinRow]->GetNextAliveRow();
#ifndef NDEBUG
		iStoredDocs++;
#endif
	}

	SafeDeleteArray ( pFixedRow );

	assert ( iStoredDocs==iTotalDocs );

	tMinMaxBuilder.FinishCollect ();
	SphOffset_t uMinMaxOff = wrRows.GetPos() / sizeof(CSphRowitem);
	if ( tMinMaxBuilder.GetActualSize() )
		wrRows.PutBytes ( dMinMaxBuffer.Begin(), tMinMaxBuilder.GetActualSize()*sizeof(DWORD) );

	tMvaWriter.CloseFile();
	tStrWriter.CloseFile ();

	////////////////////
	// write docs & hits
	////////////////////

	assert ( iMinDocID>0 );
	iMinDocID--;

	// OPTIMIZE? somehow avoid new on iterators maybe?
	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
		pWordReaders.Add ( new RtWordReader_T<SphWordID_t> ( tGuard.m_dRamChunks[i], m_bKeywordDict, m_iWordsCheckpoint ) );

	ARRAY_FOREACH ( i, pWordReaders )
		pWords.Add ( pWordReaders[i]->UnzipWord() );

	// loop keywords
	CSphVector<Checkpoint_t> dCheckpoints;
	CSphVector<BYTE> dKeywordCheckpoints;
	int iWords = 0;
	CSphKeywordDeltaWriter tLastWord;
	SphWordID_t uLastWordID = 0;
	SphOffset_t uLastDocpos = 0;
	CSphVector<SkiplistEntry_t> dSkiplist;

	bool bHasMorphology = m_pDict->HasMorphology();

	CSphScopedPtr<ISphInfixBuilder> pInfixer ( NULL );
	if ( m_tSettings.m_iMinInfixLen && m_pDict->GetSettings().m_bWordDict )
		pInfixer = sphCreateInfixBuilder ( m_pTokenizer->GetMaxCodepointLength(), &sError );

	while (true)
	{
		// find keyword with min id
		const RTWORD * pWord = NULL;
		ARRAY_FOREACH ( i, pWords ) // OPTIMIZE? PQ or at least nulls removal here?!
		{
			if ( pWords[i] )
			{
				if ( !pWord
					|| ( !m_bKeywordDict && pWords[i]->m_uWordID<pWord->m_uWordID )
					|| ( m_bKeywordDict &&
						sphDictCmpStrictly ( (const char *)pWords[i]->m_sWord+1, *pWords[i]->m_sWord, (const char *)pWord->m_sWord+1, *pWord->m_sWord )<0 ) )
				{
					pWord = pWords[i];
				}
			}
		}

		if ( !pWord )
			break;

		// loop all segments that have this keyword
		assert ( pSegments.GetLength()==0 );
		assert ( pDocReaders.GetLength()==0 );
		assert ( pDocs.GetLength()==0 );

		ARRAY_FOREACH ( i, pWords )
			if ( pWords[i] &&
				( ( !m_bKeywordDict && pWords[i]->m_uWordID==pWord->m_uWordID )
				|| ( m_bKeywordDict &&
				sphDictCmpStrictly ( (const char *)pWords[i]->m_sWord+1, *pWords[i]->m_sWord, (const char *)pWord->m_sWord+1, *pWord->m_sWord )==0 ) ) )
			{
				pSegments.Add ();
				pSegments.Last().m_pSeg = tGuard.m_dRamChunks[i];
				pSegments.Last().m_pKill = &tGuard.m_dKill[i]->m_dKilled;
				pDocReaders.Add ( new RtDocReader_T<SphDocID_t> ( tGuard.m_dRamChunks[i], *pWords[i] ) );

				const RTDOC * pDoc = pDocReaders.Last()->UnzipDoc();
				while ( pDoc && tGuard.m_dKill[i]->m_dKilled.BinarySearch ( pDoc->m_uDocID ) )
					pDoc = pDocReaders.Last()->UnzipDoc();

				pDocs.Add ( pDoc );
			}

		// loop documents
		SphOffset_t uDocpos = wrDocs.GetPos();
		SphDocID_t uLastDoc = 0;
		SphOffset_t uLastHitpos = 0;
		SphDocID_t uSkiplistDocID = iMinDocID;
		int iDocs = 0;
		int iHits = 0;
		dSkiplist.Resize ( 0 );
		while (true)
		{
			// find alive doc with min id
			int iMinReader = -1;
			ARRAY_FOREACH ( i, pDocs ) // OPTIMIZE?
			{
				if ( !pDocs[i] )
					continue;

				assert ( !pSegments[i].m_pKill->BinarySearch ( pDocs[i]->m_uDocID ) );
				if ( iMinReader<0 || pDocs[i]->m_uDocID < pDocs[iMinReader]->m_uDocID )
					iMinReader = i;
			}
			if ( iMinReader<0 )
				break;

			// write doclist entry
			const RTDOC * pDoc = pDocs[iMinReader]; // shortcut
			// build skiplist, aka save decoder state as needed
			if ( ( iDocs & ( SPH_SKIPLIST_BLOCK-1 ) )==0 )
			{
				SkiplistEntry_t & t = dSkiplist.Add();
				t.m_iBaseDocid = uSkiplistDocID;
				t.m_iOffset = wrDocs.GetPos();
				t.m_iBaseHitlistPos = uLastHitpos;
			}
			iDocs++;
			iHits += pDoc->m_uHits;
			uSkiplistDocID = pDoc->m_uDocID;

			wrDocs.ZipOffset ( pDoc->m_uDocID - uLastDoc - iMinDocID );
			wrDocs.ZipInt ( pDoc->m_uHits );
			if ( pDoc->m_uHits==1 )
			{
				wrDocs.ZipInt ( pDoc->m_uHit & 0x7FFFFFUL );
				wrDocs.ZipInt ( pDoc->m_uHit >> 23 );
			} else
			{
				wrDocs.ZipInt ( pDoc->m_uDocFields );
				wrDocs.ZipOffset ( wrHits.GetPos() - uLastHitpos );
				uLastHitpos = wrHits.GetPos();
			}

			uLastDoc = pDoc->m_uDocID - iMinDocID;

			// loop hits from most current segment
			if ( pDoc->m_uHits>1 )
			{
				DWORD uLastHit = 0;
				RtHitReader_t tInHit ( pSegments[iMinReader].m_pSeg, pDoc );
				for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
				{
					wrHits.ZipInt ( uValue - uLastHit );
					uLastHit = uValue;
				}
				wrHits.ZipInt ( 0 );
			}

			// fast forward readers
			SphDocID_t uMinID = pDocs[iMinReader]->m_uDocID;
			ARRAY_FOREACH ( i, pDocs )
				while ( pDocs[i] && ( pDocs[i]->m_uDocID<=uMinID || pSegments[i].m_pKill->BinarySearch ( pDocs[i]->m_uDocID ) ) )
					pDocs[i] = pDocReaders[i]->UnzipDoc();
		}

		// write skiplist
		int iSkiplistOff = (int)wrSkips.GetPos();
		for ( int i=1; i<dSkiplist.GetLength(); i++ )
		{
			const SkiplistEntry_t & tPrev = dSkiplist[i-1];
			const SkiplistEntry_t & tCur = dSkiplist[i];
			assert ( tCur.m_iBaseDocid - tPrev.m_iBaseDocid>=SPH_SKIPLIST_BLOCK );
			assert ( tCur.m_iOffset - tPrev.m_iOffset>=4*SPH_SKIPLIST_BLOCK );
			wrSkips.ZipOffset ( tCur.m_iBaseDocid - tPrev.m_iBaseDocid - SPH_SKIPLIST_BLOCK );
			wrSkips.ZipOffset ( tCur.m_iOffset - tPrev.m_iOffset - 4*SPH_SKIPLIST_BLOCK );
			wrSkips.ZipOffset ( tCur.m_iBaseHitlistPos - tPrev.m_iBaseHitlistPos );
		}

		// write dict entry if necessary
		if ( wrDocs.GetPos()!=uDocpos )
		{
			wrDocs.ZipInt ( 0 ); // docs over

			if ( ( iWords%SPH_WORDLIST_CHECKPOINT )==0 )
			{
				if ( iWords )
				{
					SphOffset_t uOff = m_bKeywordDict ? 0 : uDocpos - uLastDocpos;
					wrDict.ZipInt ( 0 );
					wrDict.ZipOffset ( uOff ); // store last hitlist length
				}

				// restart delta coding, once per SPH_WORDLIST_CHECKPOINT entries
				uLastDocpos = 0;
				uLastWordID = 0;
				tLastWord.Reset();

				// begin new wordlist entry
				Checkpoint_t & tChk = dCheckpoints.Add ();
				tChk.m_uOffset = wrDict.GetPos();
				if ( m_bKeywordDict )
				{
					// copy word len + word itself to checkpoint storage
					tChk.m_uWord = sphPutBytes ( &dKeywordCheckpoints, pWord->m_sWord, pWord->m_sWord[0]+1 );
				} else
				{
					tChk.m_uWord = pWord->m_uWordID;
				}
			}
			iWords++;

			if ( m_bKeywordDict )
			{
				tLastWord.PutDelta ( wrDict, pWord->m_sWord+1, pWord->m_sWord[0] );
				wrDict.ZipOffset ( uDocpos );
			} else
			{
				assert ( pWord->m_uWordID!=uLastWordID );
				wrDict.ZipOffset ( pWord->m_uWordID - uLastWordID );
				uLastWordID = pWord->m_uWordID;
				assert ( uDocpos>uLastDocpos );
				wrDict.ZipOffset ( uDocpos - uLastDocpos );
			}
			wrDict.ZipInt ( iDocs );
			wrDict.ZipInt ( iHits );
			if ( m_bKeywordDict )
			{
				BYTE uHint = sphDoclistHintPack ( iDocs, wrDocs.GetPos()-uLastDocpos );
				if ( uHint )
					wrDict.PutByte ( uHint );

				// build infixes
				if ( pInfixer.Ptr() )
					pInfixer->AddWord ( pWord->m_sWord+1, pWord->m_sWord[0], dCheckpoints.GetLength(), bHasMorphology );
			}

			// emit skiplist pointer
			if ( iDocs>SPH_SKIPLIST_BLOCK )
				wrDict.ZipInt ( iSkiplistOff );

			uLastDocpos = uDocpos;
		}

		// move words forward
		// because pWord contents will move forward too!
		SphWordID_t uMinID = pWord->m_uWordID;
		char sMinWord[SPH_MAX_KEYWORD_LEN];
		int iMinWordLen = 0;
		if ( m_bKeywordDict )
		{
			iMinWordLen = pWord->m_sWord[0];
			assert ( iMinWordLen<SPH_MAX_KEYWORD_LEN );
			memcpy ( sMinWord, pWord->m_sWord+1, iMinWordLen );
		}

		ARRAY_FOREACH ( i, pWords )
		{
			if ( pWords[i] &&
				( ( !m_bKeywordDict && pWords[i]->m_uWordID==uMinID )
				|| ( m_bKeywordDict && sphDictCmpStrictly ( (const char *)pWords[i]->m_sWord+1, pWords[i]->m_sWord[0], sMinWord, iMinWordLen )==0 ) ) )
			{
				pWords[i] = pWordReaders[i]->UnzipWord();
			}
		}

		// cleanup
		ARRAY_FOREACH ( i, pDocReaders )
			SafeDelete ( pDocReaders[i] );
		pSegments.Resize ( 0 );
		pDocReaders.Resize ( 0 );
		pDocs.Resize ( 0 );
	}

	// write checkpoints
	SphOffset_t uOff = m_bKeywordDict ? 0 : wrDocs.GetPos() - uLastDocpos;
	// FIXME!!! don't write to wrDict if iWords==0
	// however plain index becomes m_bIsEmpty and full scan does not work there
	// we'll get partly working RT ( RAM chunk works and disk chunks give empty result set )
	wrDict.ZipInt ( 0 ); // indicate checkpoint
	wrDict.ZipOffset ( uOff ); // store last doclist length

	// flush infix hash entries, if any
	if ( pInfixer.Ptr() )
		pInfixer->SaveEntries ( wrDict );

	SphOffset_t iCheckpointsPosition = wrDict.GetPos();
	if ( m_bKeywordDict )
	{
		const char * pCheckpoints = (const char *)dKeywordCheckpoints.Begin();
		ARRAY_FOREACH ( i, dCheckpoints )
		{
			const char * pPacked = pCheckpoints + dCheckpoints[i].m_uWord;
			int iLen = *pPacked;
			assert ( iLen && (int)dCheckpoints[i].m_uWord+1+iLen<=dKeywordCheckpoints.GetLength() );
			wrDict.PutDword ( iLen );
			wrDict.PutBytes ( pPacked+1, iLen );
			wrDict.PutOffset ( dCheckpoints[i].m_uOffset );
		}
	} else
	{
		ARRAY_FOREACH ( i, dCheckpoints )
		{
			wrDict.PutOffset ( dCheckpoints[i].m_uWord );
			wrDict.PutOffset ( dCheckpoints[i].m_uOffset );
		}
	}

	int64_t iInfixBlockOffset = 0;
	int iInfixCheckpointWordsSize = 0;
	// flush infix hash blocks
	if ( pInfixer.Ptr() )
	{
		iInfixBlockOffset = pInfixer->SaveEntryBlocks ( wrDict );
		iInfixCheckpointWordsSize = pInfixer->GetBlocksWordsSize();

		if ( iInfixBlockOffset>UINT_MAX )
			sphWarning ( "INTERNAL ERROR: dictionary size " INT64_FMT " overflow at infix save", iInfixBlockOffset );
	}

	// flush header
	// mostly for debugging convenience
	// primary storage is in the index wide header
	wrDict.PutBytes ( "dict-header", 11 );
	wrDict.ZipInt ( dCheckpoints.GetLength() );
	wrDict.ZipOffset ( iCheckpointsPosition );
	wrDict.ZipInt ( m_pTokenizer->GetMaxCodepointLength() );
	wrDict.ZipInt ( (DWORD)iInfixBlockOffset );

	// write dummy kill-list files
	CSphWriter wrDummy;
	// dump killlist
	sName.SetSprintf ( "%s.spk", sFilename );
	wrDummy.OpenFile ( sName.cstr(), sError );
	if ( m_dDiskChunkKlist.GetLength() )
		wrDummy.PutBytes ( m_dDiskChunkKlist.Begin(), m_dDiskChunkKlist.GetLength()*sizeof ( SphDocID_t ) );
	wrDummy.CloseFile ();

	// header
	SaveDiskHeader ( sFilename, iMinDocID, dCheckpoints.GetLength(), iCheckpointsPosition, (DWORD)iInfixBlockOffset, iInfixCheckpointWordsSize,
		m_dDiskChunkKlist.GetLength(), uMinMaxOff, tStats, iTotalDocs );

	// cleanup
	ARRAY_FOREACH ( i, pWordReaders )
		SafeDelete ( pWordReaders[i] );
	ARRAY_FOREACH ( i, pDocReaders )
		SafeDelete ( pDocReaders[i] );
	ARRAY_FOREACH ( i, pRowIterators )
		SafeDelete ( pRowIterators[i] );

	// done
	wrSkips.CloseFile ();
	wrHits.CloseFile ();
	wrDocs.CloseFile ();
	wrDict.CloseFile ();
	wrRows.CloseFile ();
}


void RtIndex_t::SaveDiskHeader ( const char * sFilename, SphDocID_t iMinDocID, int iCheckpoints,
	SphOffset_t iCheckpointsPosition, DWORD iInfixBlocksOffset, int iInfixCheckpointWordsSize, DWORD uKillListSize, uint64_t uMinMaxSize,
	const ChunkStats_t & tStats, int64_t iTotalDocuments ) const
{
	static const DWORD RT_INDEX_FORMAT_VERSION	= 43;			///< my format version

	CSphWriter tWriter;
	CSphString sName, sError;
	sName.SetSprintf ( "%s.sph", sFilename );
	tWriter.OpenFile ( sName.cstr(), sError );

	// format
	tWriter.PutDword ( INDEX_MAGIC_HEADER );
	tWriter.PutDword ( RT_INDEX_FORMAT_VERSION );

	tWriter.PutDword ( 1 ); // use-64bit
	tWriter.PutDword ( SPH_DOCINFO_EXTERN );

	// schema
	WriteSchema ( tWriter, m_tSchema );

	// min docid
	tWriter.PutOffset ( iMinDocID );

	// wordlist checkpoints
	tWriter.PutOffset ( iCheckpointsPosition );
	tWriter.PutDword ( iCheckpoints );

	int iInfixCodepointBytes = ( m_tSettings.m_iMinInfixLen && m_pDict->GetSettings().m_bWordDict ? m_pTokenizer->GetMaxCodepointLength() : 0 );
	tWriter.PutByte ( iInfixCodepointBytes ); // m_iInfixCodepointBytes, v.27+
	tWriter.PutDword ( iInfixBlocksOffset ); // m_iInfixBlocksOffset, v.27+
	tWriter.PutDword ( iInfixCheckpointWordsSize ); // m_iInfixCheckpointWordsSize, v.34+

	// stats
	tWriter.PutDword ( (DWORD)iTotalDocuments ); // FIXME? we don't expect over 4G docs per just 1 local index
	tWriter.PutOffset ( tStats.m_Stats.m_iTotalBytes );
	// FIXME!!! calc duplicates here to
	tWriter.PutDword ( 0 ); // v.40+

	// index settings
	tWriter.PutDword ( m_tSettings.m_iMinPrefixLen );
	tWriter.PutDword ( m_tSettings.m_iMinInfixLen );
	tWriter.PutDword ( m_tSettings.m_iMaxSubstringLen );
	tWriter.PutByte ( m_tSettings.m_bHtmlStrip ? 1 : 0 );
	tWriter.PutString ( m_tSettings.m_sHtmlIndexAttrs.cstr () );
	tWriter.PutString ( m_tSettings.m_sHtmlRemoveElements.cstr () );
	tWriter.PutByte ( m_tSettings.m_bIndexExactWords ? 1 : 0 );
	tWriter.PutDword ( m_tSettings.m_eHitless );
	tWriter.PutDword ( SPH_HIT_FORMAT_INLINE );
	tWriter.PutByte ( m_tSettings.m_bIndexSP ? 1 : 0 ); // m_bIndexSP, v.21+
	tWriter.PutString ( m_tSettings.m_sZones ); // m_sZonePrefix, v.22+
	tWriter.PutDword ( 0 ); // m_iBoundaryStep, v.23+
	tWriter.PutDword ( 1 ); // m_iStopwordStep, v.23+
	tWriter.PutDword ( 1 );	// m_iOvershortStep
	tWriter.PutDword ( m_tSettings.m_iEmbeddedLimit );	// v.30+
	tWriter.PutByte ( m_tSettings.m_eBigramIndex ); // v.32+
	tWriter.PutString ( m_tSettings.m_sBigramWords ); // v.32+
	tWriter.PutByte ( m_tSettings.m_bIndexFieldLens ); // v. 35+
	tWriter.PutByte ( m_tSettings.m_eChineseRLP ); // v. 39+
	tWriter.PutString ( m_tSettings.m_sRLPContext ); // v. 39+
	tWriter.PutString ( m_tSettings.m_sIndexTokenFilter ); // v.41+

	// tokenizer
	SaveTokenizerSettings ( tWriter, m_pTokenizer, m_tSettings.m_iEmbeddedLimit );

	// dictionary
	// can not use embedding as stopwords id differs between RT and plain dictionaries
	SaveDictionarySettings ( tWriter, m_pDict, m_bKeywordDict, 0 );

	// kill-list size
	tWriter.PutDword ( uKillListSize );

	// min-max count
	tWriter.PutOffset ( uMinMaxSize );

	// field filter
	SaveFieldFilterSettings ( tWriter, m_pFieldFilter );

	// field lengths
	if ( m_tSettings.m_bIndexFieldLens )
		for ( int i=0; i <m_tSchema.GetFieldsCount(); i++ )
			tWriter.PutOffset ( tStats.m_dFieldLens[i] );

	// done
	tWriter.CloseFile ();
}

namespace sph
{
	int rename ( const char * sOld, const char * sNew )
	{
#if USE_WINDOWS
		if ( MoveFileEx ( sOld, sNew, MOVEFILE_REPLACE_EXISTING ) )
			return 0;
		errno = GetLastError();
		return -1;
#else
		return ::rename ( sOld, sNew );
#endif
	}
}

void RtIndex_t::SaveMeta ( int64_t iTID, const CSphFixedVector<int> & dChunkNames )
{
	// sanity check
	if ( m_iLockFD<0 )
		return;

	// write new meta
	CSphString sMeta, sMetaNew;
	sMeta.SetSprintf ( "%s.meta", m_sPath.cstr() );
	sMetaNew.SetSprintf ( "%s.meta.new", m_sPath.cstr() );

	CSphString sError;
	CSphWriter wrMeta;
	if ( !wrMeta.OpenFile ( sMetaNew, sError ) )
		sphDie ( "failed to serialize meta: %s", sError.cstr() ); // !COMMIT handle this gracefully
	wrMeta.PutDword ( META_HEADER_MAGIC );
	wrMeta.PutDword ( META_VERSION );
	wrMeta.PutDword ( dChunkNames.GetLength() );
	wrMeta.PutDword ( 0 );
	wrMeta.PutDword ( (DWORD)m_tStats.m_iTotalDocuments ); // FIXME? we don't expect over 4G docs per just 1 local index
	wrMeta.PutOffset ( m_tStats.m_iTotalBytes ); // FIXME? need PutQword ideally
	wrMeta.PutOffset ( iTID );

	// meta v.4, save disk index format and settings, too
	wrMeta.PutDword ( INDEX_FORMAT_VERSION );
	WriteSchema ( wrMeta, m_tSchema );
	SaveIndexSettings ( wrMeta, m_tSettings );
	SaveTokenizerSettings ( wrMeta, m_pTokenizer, m_tSettings.m_iEmbeddedLimit );
	SaveDictionarySettings ( wrMeta, m_pDict, m_bKeywordDict, m_tSettings.m_iEmbeddedLimit );

	// meta v.5
	wrMeta.PutDword ( m_iWordsCheckpoint );

	// meta v.7
	wrMeta.PutDword ( m_iMaxCodepointLength );
	wrMeta.PutByte ( BLOOM_PER_ENTRY_VALS_COUNT );
	wrMeta.PutByte ( BLOOM_HASHES_COUNT );

	// meta v.11
	SaveFieldFilterSettings ( wrMeta, m_pFieldFilter );

	// meta v.12
	wrMeta.PutDword ( dChunkNames.GetLength () );
	wrMeta.PutBytes ( dChunkNames.Begin(), dChunkNames.GetLengthBytes() );

	wrMeta.CloseFile(); // FIXME? handle errors?

	// rename
	if ( sph::rename ( sMetaNew.cstr(), sMeta.cstr() ) )
		sphDie ( "failed to rename meta (src=%s, dst=%s, errno=%d, error=%s)",
			sMetaNew.cstr(), sMeta.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully
}


void RtIndex_t::SaveDiskChunk ( int64_t iTID, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats, bool bMoveRetired )
{
	if ( !tGuard.m_dRamChunks.GetLength() )
		return;

	MEMORY ( MEM_INDEX_RT );

	CSphFixedVector<int> dChunkNames = GetIndexNames ( tGuard.m_dDiskChunks, true );

	// dump it
	CSphString sNewChunk;
	sNewChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), dChunkNames.Last() );
	SaveDiskDataImpl ( sNewChunk.cstr(), tGuard, tStats );

	// bring new disk chunk online
	CSphIndex * pDiskChunk = LoadDiskChunk ( sNewChunk.cstr(), m_sLastError );
	if ( !pDiskChunk )
		sphDie ( "%s", m_sLastError.cstr() );

	// FIXME! add binlog cleanup here once we have binlogs

	// get exclusive lock again, gotta reset RAM chunk now
	Verify ( m_tWriting.Lock() );
	Verify ( m_tChunkLock.WriteLock() );

	// save updated meta
	SaveMeta ( iTID, dChunkNames );
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// swap double buffer data
	int iNewSegmentsCount = ( m_iDoubleBuffer ? m_dRamChunks.GetLength() - m_iDoubleBuffer : 0 );
	for ( int i=0; i<iNewSegmentsCount; i++ )
		m_dRamChunks[i] = m_dRamChunks[i+m_iDoubleBuffer];
	m_dRamChunks.Resize ( iNewSegmentsCount );

	m_dDiskChunks.Add ( pDiskChunk );

	// update field lengths
	if ( m_tSchema.GetAttrId_FirstFieldLen()>=0 )
	{
		ARRAY_FOREACH ( i, m_dFieldLensRam )
			m_dFieldLensRam[i] -= tStats.m_dFieldLens[i];
		ARRAY_FOREACH ( i, m_dFieldLensDisk )
			m_dFieldLensDisk[i] += tStats.m_dFieldLens[i];
	}

	// move up kill-list
	m_tKlist.Reset ( m_dNewSegmentKlist.Begin(), m_dNewSegmentKlist.GetLength() );
	m_dNewSegmentKlist.Reset();
	m_dDiskChunkKlist.Reset();

	Verify ( m_tChunkLock.Unlock() );

	if ( bMoveRetired )
	{
		ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
			m_dRetired.Add ( tGuard.m_dRamChunks[i] );
	}

	// abandon .ram file
	CSphString sChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );
	if ( sphIsReadable ( sChunk.cstr() ) && ::unlink ( sChunk.cstr() ) )
		sphWarning ( "failed to unlink ram chunk (file=%s, errno=%d, error=%s)", sChunk.cstr(), errno, strerrorm(errno) );

	FreeRetired();

	m_iDoubleBuffer = 0;
	m_iSavedTID = iTID;
	m_tmSaved = sphMicroTimer();

	Verify ( m_tWriting.Unlock() );
}


CSphIndex * RtIndex_t::LoadDiskChunk ( const char * sChunk, CSphString & sError ) const
{
	MEMORY ( MEM_INDEX_DISK );

	// !COMMIT handle errors gracefully instead of dying
	CSphIndex * pDiskChunk = sphCreateIndexPhrase ( sChunk, sChunk );
	if ( !pDiskChunk )
	{
		sError.SetSprintf ( "disk chunk %s: alloc failed", sChunk );
		return NULL;
	}

	pDiskChunk->m_iExpansionLimit = m_iExpansionLimit;
	pDiskChunk->m_iExpandKeywords = m_iExpandKeywords;
	pDiskChunk->SetBinlog ( false );
	pDiskChunk->SetMemorySettings ( m_bMlock, m_bOndiskAllAttr, m_bOndiskPoolAttr );

	if ( !pDiskChunk->Prealloc ( m_bPathStripped ) )
	{
		sError.SetSprintf ( "disk chunk %s: prealloc failed: %s", sChunk, pDiskChunk->GetLastError().cstr() );
		SafeDelete ( pDiskChunk );
		return NULL;
	}
	pDiskChunk->Preread();

	return pDiskChunk;
}


bool RtIndex_t::Prealloc ( bool bStripPath )
{
	MEMORY ( MEM_INDEX_RT );

	// locking uber alles
	// in RT backend case, we just must be multi-threaded
	// so we simply lock here, and ignore Lock/Unlock hassle caused by forks
	assert ( m_iLockFD<0 );

	CSphString sLock;
	sLock.SetSprintf ( "%s.lock", m_sPath.cstr() );
	m_iLockFD = ::open ( sLock.cstr(), SPH_O_NEW, 0644 );
	if ( m_iLockFD<0 )
	{
		m_sLastError.SetSprintf ( "failed to open %s: %s", sLock.cstr(), strerrorm(errno) );
		return false;
	}
	if ( !sphLockEx ( m_iLockFD, false ) )
	{
		m_sLastError.SetSprintf ( "failed to lock %s: %s", sLock.cstr(), strerrorm(errno) );
		::close ( m_iLockFD );
		return false;
	}

	/////////////
	// load meta
	/////////////

	// check if we have a meta file (kinda-header)
	CSphString sMeta;
	sMeta.SetSprintf ( "%s.meta", m_sPath.cstr() );

	// no readable meta? no disk part yet
	if ( !sphIsReadable ( sMeta.cstr() ) )
		return true;

	// opened and locked, lets read
	CSphAutoreader rdMeta;
	if ( !rdMeta.Open ( sMeta, m_sLastError ) )
		return false;

	if ( rdMeta.GetDword()!=META_HEADER_MAGIC )
	{
		m_sLastError.SetSprintf ( "invalid meta file %s", sMeta.cstr() );
		return false;
	}
	DWORD uVersion = rdMeta.GetDword();
	if ( uVersion==0 || uVersion>META_VERSION )
	{
		m_sLastError.SetSprintf ( "%s is v.%d, binary is v.%d", sMeta.cstr(), uVersion, META_VERSION );
		return false;
	}
	const int iDiskChunks = rdMeta.GetDword();
	int iDiskBase = 0;
	if ( uVersion>=6 )
		iDiskBase = rdMeta.GetDword();
	m_tStats.m_iTotalDocuments = rdMeta.GetDword();
	m_tStats.m_iTotalBytes = rdMeta.GetOffset();
	if ( uVersion>=2 )
		m_iTID = rdMeta.GetOffset();

	// tricky bit
	// we started saving settings into .meta from v.4 and up only
	// and those reuse disk format version, aka INDEX_FORMAT_VERSION
	// anyway, starting v.4, serialized settings take precedence over config
	// so different chunks can't have different settings any more
	CSphTokenizerSettings tTokenizerSettings;
	if ( uVersion>=4 )
	{
		CSphDictSettings tDictSettings;
		CSphEmbeddedFiles tEmbeddedFiles;
		CSphString sWarning;

		// load them settings
		DWORD uSettingsVer = rdMeta.GetDword();
		ReadSchema ( rdMeta, m_tSchema, uSettingsVer, false );
		LoadIndexSettings ( m_tSettings, rdMeta, uSettingsVer );
		if ( !LoadTokenizerSettings ( rdMeta, tTokenizerSettings, tEmbeddedFiles, uSettingsVer, m_sLastError ) )
			return false;
		LoadDictionarySettings ( rdMeta, tDictSettings, tEmbeddedFiles, uSettingsVer, sWarning );

		// meta v.5 dictionary
		if ( uVersion>=5 )
			m_bKeywordDict = tDictSettings.m_bWordDict;

		// initialize AOT if needed
		DWORD uPrevAot = m_tSettings.m_uAotFilterMask;
		m_tSettings.m_uAotFilterMask = sphParseMorphAot ( tDictSettings.m_sMorphology.cstr() );
		if ( m_tSettings.m_uAotFilterMask!=uPrevAot )
			sphWarning ( "index '%s': morphology option changed from config has no effect, ignoring", m_sIndexName.cstr() );

		if ( bStripPath )
		{
			StripPath ( tTokenizerSettings.m_sSynonymsFile );
			StripPath ( tDictSettings.m_sStopwords );
			ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
				StripPath ( tDictSettings.m_dWordforms[i] );
		}

		// recreate tokenizer
		m_pTokenizer = ISphTokenizer::Create ( tTokenizerSettings, &tEmbeddedFiles, m_sLastError );
		if ( !m_pTokenizer )
			return false;

		// recreate dictionary
		m_pDict = sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, m_pTokenizer, m_sIndexName.cstr(), m_sLastError );
		if ( !m_pDict )
		{
			m_sLastError.SetSprintf ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );
			return false;
		}

		m_pTokenizer = ISphTokenizer::CreateMultiformFilter ( m_pTokenizer, m_pDict->GetMultiWordforms () );

		// update schema
		m_iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	}

	// meta v.5 checkpoint freq
	m_iWordsCheckpoint = ( uVersion<5 ? RTDICT_CHECKPOINT_V3 : RTDICT_CHECKPOINT_V5 );
	if ( uVersion>=5 )
	{
		m_iWordsCheckpoint = rdMeta.GetDword();
	}

	// check that infixes definition changed - going to rebuild infixes
	bool bRebuildInfixes = false;
	if ( uVersion>=7 )
	{
		m_iMaxCodepointLength = rdMeta.GetDword();
		int iBloomKeyLen = rdMeta.GetByte();
		int iBloomHashesCount = rdMeta.GetByte();
		bRebuildInfixes = ( iBloomKeyLen!=BLOOM_PER_ENTRY_VALS_COUNT || iBloomHashesCount!=BLOOM_HASHES_COUNT );

		if ( bRebuildInfixes )
			sphWarning ( "infix definition changed (from len=%d, hashes=%d to len=%d, hashes=%d) - rebuilding...",
						(int)BLOOM_PER_ENTRY_VALS_COUNT, (int)BLOOM_HASHES_COUNT, iBloomKeyLen, iBloomHashesCount );
	}

	if ( uVersion>=11 )
	{
		ISphFieldFilterRefPtr_c pFieldFilter;
		CSphFieldFilterSettings tFieldFilterSettings;
		LoadFieldFilterSettings ( rdMeta, tFieldFilterSettings );
		if ( tFieldFilterSettings.m_dRegexps.GetLength() )
			pFieldFilter = sphCreateRegexpFilter ( tFieldFilterSettings, m_sLastError );

		if ( !sphSpawnRLPFilter ( pFieldFilter, m_tSettings, tTokenizerSettings, sMeta.cstr(), m_sLastError ) )
			return false;

		SetFieldFilter ( pFieldFilter );
	}

	CSphFixedVector<int> dChunkNames(0);
	if ( uVersion>=12 )
	{
		int iLen = (int)rdMeta.GetDword();
		dChunkNames.Reset ( iLen );
		rdMeta.GetBytes ( dChunkNames.Begin(), iLen*sizeof(int) );
	}

	// prior to v.12 use iDiskBase + iDiskChunks
	// v.12 stores chunk list but wrong
	if ( uVersion<13 )
	{
		dChunkNames.Reset ( iDiskChunks );
		ARRAY_FOREACH ( iChunk, dChunkNames )
			dChunkNames[iChunk] = iChunk + iDiskBase;
	}

	///////////////
	// load chunks
	///////////////

	m_bPathStripped = bStripPath;

	// load disk chunks, if any
	ARRAY_FOREACH ( iChunk, dChunkNames )
	{
		CSphString sChunk;
		sChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), dChunkNames[iChunk] );
		CSphIndex * pIndex = LoadDiskChunk ( sChunk.cstr(), m_sLastError );
		if ( !pIndex )
			sphDie ( "%s", m_sLastError.cstr() );

		m_dDiskChunks.Add ( pIndex );

		// tricky bit
		// outgoing match schema on disk chunk should be identical to our internal (!) schema
		if ( !m_tSchema.CompareTo ( pIndex->GetMatchSchema(), m_sLastError ) )
			return false;

		// update field lengths
		if ( m_tSchema.GetAttrId_FirstFieldLen()>=0 )
		{
			int64_t * pLens = pIndex->GetFieldLens();
			if ( pLens )
				for ( int i=0; i < pIndex->GetMatchSchema().GetFieldsCount(); i++ )
					m_dFieldLensDisk[i] += pLens[i];
		}
	}

	// load ram chunk
	bool bRamLoaded = LoadRamChunk ( uVersion, bRebuildInfixes );

	// field lengths
	ARRAY_FOREACH ( i, m_dFieldLens )
		m_dFieldLens[i] = m_dFieldLensDisk[i] + m_dFieldLensRam[i];

	// set up values for on timer save
	m_iSavedTID = m_iTID;
	m_tmSaved = sphMicroTimer();

	return bRamLoaded;
}


void RtIndex_t::Preread ()
{
	// !COMMIT move disk chunks prereading here
}


void RtIndex_t::SetMemorySettings ( bool bMlock, bool bOndiskAttrs, bool bOndiskPool )
{
	m_bMlock = bMlock;
	m_bOndiskAllAttr = bOndiskAttrs;
	m_bOndiskPoolAttr = ( bOndiskAttrs || bOndiskPool );
}


static bool CheckVectorLength ( int iLen, int64_t iSaneLen, const char * sAt, CSphString & sError )
{
	if ( iLen>=0 && iLen<iSaneLen )
		return true;

	sError.SetSprintf ( "broken index, %s length overflow (len=%d, max=" INT64_FMT ")", sAt, iLen, iSaneLen );
	return false;
}

template < typename T, typename P >
static void SaveVector ( CSphWriter & tWriter, const CSphVector < T, P > & tVector )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	tWriter.PutDword ( tVector.GetLength() );
	if ( tVector.GetLength() )
		tWriter.PutBytes ( tVector.Begin(), tVector.GetLengthBytes() );
}


template < typename T, typename P >
static bool LoadVector ( CSphReader & tReader, CSphVector < T, P > & tVector,
	int64_t iSaneLen, const char * sAt, CSphString & sError )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	int iSize = tReader.GetDword();
	if ( !CheckVectorLength ( iSize, iSaneLen, sAt, sError ) )
		return false;

	tVector.Resize ( iSize );
	if ( tVector.GetLength() )
		tReader.GetBytes ( tVector.Begin(), tVector.GetLengthBytes() );

	return true;
}


template < typename T, typename P >
static void SaveVector ( BinlogWriter_c &tWriter, const CSphVector<T, P> &tVector )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	tWriter.ZipOffset ( tVector.GetLength() );
	if ( tVector.GetLength() )
		tWriter.PutBytes ( tVector.Begin(), tVector.GetLengthBytes() );
}


template < typename T, typename P >
static bool LoadVector ( BinlogReader_c & tReader, CSphVector < T, P > & tVector )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	tVector.Resize ( (int) tReader.UnzipOffset() ); // FIXME? sanitize?
	if ( tVector.GetLength() )
		tReader.GetBytes ( tVector.Begin(), tVector.GetLengthBytes() );
	return !tReader.GetErrorFlag();
}


bool RtIndex_t::SaveRamChunk ()
{
	MEMORY ( MEM_INDEX_RT );

	CSphString sChunk, sNewChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );
	sNewChunk.SetSprintf ( "%s.ram.new", m_sPath.cstr() );
	m_tKlist.SaveToFile ( m_sPath.cstr() );

	CSphWriter wrChunk;
	if ( !wrChunk.OpenFile ( sNewChunk, m_sLastError ) )
		return false;

	wrChunk.PutDword ( 1 ); // was USE_64BIT
	wrChunk.PutDword ( RtSegment_t::m_iSegments );
	wrChunk.PutDword ( m_dRamChunks.GetLength() );

	// no locks here, because it's only intended to be called from dtor
	ARRAY_FOREACH ( iSeg, m_dRamChunks )
	{
		const RtSegment_t * pSeg = m_dRamChunks[iSeg];
		wrChunk.PutDword ( pSeg->m_iTag );
		SaveVector ( wrChunk, pSeg->m_dWords );
		if ( m_bKeywordDict )
		{
			SaveVector ( wrChunk, pSeg->m_dKeywordCheckpoints );
		}

		const char * pCheckpoints = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
		wrChunk.PutDword ( pSeg->m_dWordCheckpoints.GetLength() );
		ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
		{
			wrChunk.PutOffset ( pSeg->m_dWordCheckpoints[i].m_iOffset );
			if ( m_bKeywordDict )
			{
				wrChunk.PutOffset ( pSeg->m_dWordCheckpoints[i].m_sWord-pCheckpoints );
			} else
			{
				wrChunk.PutOffset ( pSeg->m_dWordCheckpoints[i].m_uWordID );
			}
		}
		SaveVector ( wrChunk, pSeg->m_dDocs );
		SaveVector ( wrChunk, pSeg->m_dHits );
		wrChunk.PutDword ( pSeg->m_iRows );
		wrChunk.PutDword ( pSeg->m_iAliveRows );
		SaveVector ( wrChunk, pSeg->m_dRows );

		wrChunk.PutDword ( pSeg->GetKlist().GetLength() );
		if ( pSeg->GetKlist().GetLength() )
			wrChunk.PutBytes ( pSeg->GetKlist().Begin(), sizeof(pSeg->GetKlist()[0]) * pSeg->GetKlist().GetLength() );

		SaveVector ( wrChunk, pSeg->m_dStrings );
		SaveVector ( wrChunk, pSeg->m_dMvas );

		// infixes
		SaveVector ( wrChunk, pSeg->m_dInfixFilterCP );
	}

	// field lengths
	wrChunk.PutDword ( m_tSchema.GetFieldsCount() );
	for ( int i=0; i < m_tSchema.GetFieldsCount(); i++ )
		wrChunk.PutOffset ( m_dFieldLensRam[i] );

	wrChunk.CloseFile();
	if ( wrChunk.IsError() )
		return false;

	// rename
	if ( sph::rename ( sNewChunk.cstr(), sChunk.cstr() ) )
		sphDie ( "failed to rename ram chunk (src=%s, dst=%s, errno=%d, error=%s)",
			sNewChunk.cstr(), sChunk.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully

	return true;
}


bool RtIndex_t::LoadRamChunk ( DWORD uVersion, bool bRebuildInfixes )
{
	MEMORY ( MEM_INDEX_RT );

	CSphString sChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );

	if ( !sphIsReadable ( sChunk.cstr(), &m_sLastError ) )
		return true;

	m_bLoadRamPassedOk = false;
	m_tKlist.LoadFromFile ( m_sPath.cstr() );

	CSphAutoreader rdChunk;
	if ( !rdChunk.Open ( sChunk, m_sLastError ) )
		return false;

	if ( !rdChunk.GetDword () ) // !Id64
	{
		m_sLastError = "indexes with 32-bit docids are no longer supported";
		return false;
	}

	int64_t iFileSize = rdChunk.GetFilesize();
	int64_t iSaneVecSize = Min ( iFileSize, INT_MAX / 2 );
	int64_t iSaneTightVecSize = Min ( iFileSize, int ( INT_MAX / 1.2f ) );

	bool bHasMorphology = ( m_pDict && m_pDict->HasMorphology() ); // fresh and old-format index still has no dictionary at this point
	int iSegmentSeq = rdChunk.GetDword();

	int iSegmentCount = rdChunk.GetDword();
	if ( !CheckVectorLength ( iSegmentCount, iSaneVecSize, "ram-chunks", m_sLastError ) )
		return false;
	m_dRamChunks.Resize ( iSegmentCount );
	m_dRamChunks.Fill ( NULL );

	ARRAY_FOREACH ( iSeg, m_dRamChunks )
	{
		RtSegment_t * pSeg = new RtSegment_t ();
		m_dRamChunks[iSeg] = pSeg;

		pSeg->m_iTag = rdChunk.GetDword ();
		if ( !LoadVector ( rdChunk, pSeg->m_dWords, iSaneTightVecSize, "ram-words", m_sLastError ) )
			return false;

		if ( uVersion>=5 && m_bKeywordDict && !LoadVector ( rdChunk, pSeg->m_dKeywordCheckpoints, iSaneVecSize, "ram-checkpoints", m_sLastError ) )
				return false;

		auto * pCheckpoints = (const char *)pSeg->m_dKeywordCheckpoints.Begin();

		int iCheckpointCount = rdChunk.GetDword();
		if ( !CheckVectorLength ( iCheckpointCount, iSaneVecSize, "ram-checkpoints", m_sLastError ) )
			return false;

		pSeg->m_dWordCheckpoints.Resize ( iCheckpointCount );
		ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
		{
			pSeg->m_dWordCheckpoints[i].m_iOffset = (int)rdChunk.GetOffset();
			SphOffset_t uOff = rdChunk.GetOffset();
			if ( m_bKeywordDict )
			{
				pSeg->m_dWordCheckpoints[i].m_sWord = pCheckpoints + uOff;
			} else
			{
				pSeg->m_dWordCheckpoints[i].m_uWordID = (SphWordID_t)uOff;
			}
		}
		if ( !LoadVector ( rdChunk, pSeg->m_dDocs, iSaneTightVecSize, "ram-doclist", m_sLastError ) )
			return false;

		if ( !LoadVector ( rdChunk, pSeg->m_dHits, iSaneTightVecSize, "ram-hitlist", m_sLastError ) )
			return false;

		pSeg->m_iRows = rdChunk.GetDword();
		pSeg->m_iAliveRows = rdChunk.GetDword();
		// warning! m_dRows saved in id32 is NOT consistent for id64!
		// (the Stride for id32 is 1 DWORD shorter than for id64)
		// the only usage of this BLOB is to save id32 disk-chunk.
		if ( !LoadVector ( rdChunk, pSeg->m_dRows, iSaneTightVecSize, "ram-attributes", m_sLastError ) )
			return false;

		if ( uVersion>=9 )
		{
			int iLen = rdChunk.GetDword();
			if ( !CheckVectorLength ( iLen, Min ( iFileSize, INT_MAX ), "ram-killlist", m_sLastError ) )
				return false;

			if ( iLen )
			{
				pSeg->m_pKlist->m_dKilled.Reset ( iLen );
				rdChunk.GetBytes ( pSeg->m_pKlist->m_dKilled.Begin(), sizeof(pSeg->m_pKlist->m_dKilled[0]) * iLen );
			}
		} else
		{
			// legacy path - all types of kill-list loading either with conversion or not
			if ( uVersion==8 )
				rdChunk.GetDword(); // Hash.used

			int iLen = rdChunk.GetDword();
			if ( !CheckVectorLength ( iLen, iSaneVecSize, "ram-killlist", m_sLastError ) )
				return false;

			CSphVector<SphDocID_t> dLegacy;
			for ( int i=0; i<iLen; i++ )
			{
				SphDocID_t uDocid = rdChunk.GetOffset();
				if ( uDocid ) // hash might have 0
					dLegacy.Add ( uDocid );
			}

			if ( dLegacy.GetLength() )
			{
				dLegacy.Uniq(); // hash was unordered
				pSeg->m_pKlist->m_dKilled.CopyFrom ( dLegacy );
			}
		}

		if ( !LoadVector ( rdChunk, pSeg->m_dStrings, iSaneTightVecSize, "ram-strings", m_sLastError ) )
			return false;
		if ( uVersion>=3 && !LoadVector ( rdChunk, pSeg->m_dMvas, iSaneTightVecSize, "ram-mva", m_sLastError ) )
			return false;

		// infixes
		if ( uVersion>=7 )
		{
			if ( !LoadVector ( rdChunk, pSeg->m_dInfixFilterCP, iSaneTightVecSize, "ram-infixes", m_sLastError ) )
				return false;
			if ( bRebuildInfixes )
				BuildSegmentInfixes ( pSeg, bHasMorphology, m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ) );
		}
	}

	// field lengths
	if ( uVersion>=10 )
	{
		int iFields = rdChunk.GetDword();
		assert ( iFields==m_tSchema.GetFieldsCount() );

		for ( int i=0; i<iFields; i++ )
			m_dFieldLensRam[i] = rdChunk.GetOffset();
	}

	// all done
	RtSegment_t::m_iSegments = iSegmentSeq;
	if ( rdChunk.GetErrorFlag() )
		return false;
	m_bLoadRamPassedOk = true;
	return true;
}


void RtIndex_t::PostSetup()
{
	ISphRtIndex::PostSetup();

	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();

	// bigram filter
	if ( m_tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE && m_tSettings.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		m_pTokenizer->SetBuffer ( (BYTE*)m_tSettings.m_sBigramWords.cstr(), m_tSettings.m_sBigramWords.Length() );

		BYTE * pTok = NULL;
		while ( ( pTok = m_pTokenizer->GetToken() )!=NULL )
			m_tSettings.m_dBigramWords.Add() = (const char*)pTok;

		m_tSettings.m_dBigramWords.Sort();
	}

	// FIXME!!! handle error
	m_pTokenizerIndexing = m_pTokenizer->Clone ( SPH_CLONE_INDEX );
	ISphTokenizerRefPtr_c pIndexing { ISphTokenizer::CreateBigramFilter ( m_pTokenizerIndexing, m_tSettings.m_eBigramIndex, m_tSettings.m_sBigramWords, m_sLastError ) };
	if ( pIndexing )
		m_pTokenizerIndexing = pIndexing;

	const CSphDictSettings & tDictSettings = m_pDict->GetSettings();
	if ( !ParseMorphFields ( tDictSettings.m_sMorphology, tDictSettings.m_sMorphFields, m_tSchema.GetFields(), m_tMorphFields, m_sLastError ) )
		sphWarning ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );
}


#define LOC_FAIL(_args) \
	if ( ++iFails<=FAILS_THRESH ) \
{ \
	fprintf ( fp, "FAILED, " ); \
	fprintf _args; \
	fprintf ( fp, "\n" ); \
	iFailsPrinted++; \
	\
	if ( iFails==FAILS_THRESH ) \
	fprintf ( fp, "(threshold reached; suppressing further output)\n" ); \
}

int RtIndex_t::DebugCheck ( FILE * fp )
{
	const int FAILS_THRESH = 100;
	int iFails = 0;
	int iFailsPrinted = 0;
	int iFailsPlain = 0;

	int64_t tmCheck = sphMicroTimer();

	if ( m_iStride!=DOCINFO_IDSIZE+m_tSchema.GetRowSize() )
		LOC_FAIL(( fp, "wrong attribute stride (current=%d, should_be=%d)", m_iStride, DOCINFO_IDSIZE+m_tSchema.GetRowSize() ));

	if ( m_iSoftRamLimit<=0 )
		LOC_FAIL(( fp, "wrong RAM limit (current=" INT64_FMT ")", m_iSoftRamLimit ));

	if ( m_iLockFD<0 )
		LOC_FAIL(( fp, "index lock file id < 0" ));

	if ( m_iTID<0 )
		LOC_FAIL(( fp, "index TID < 0 (current=" INT64_FMT ")", m_iTID ));

	if ( m_iSavedTID<0 )
		LOC_FAIL(( fp, "index saved TID < 0 (current=" INT64_FMT ")", m_iSavedTID ));

	if ( m_iTID<m_iSavedTID )
		LOC_FAIL(( fp, "index TID < index saved TID (current=" INT64_FMT ", saved=" INT64_FMT ")", m_iTID, m_iSavedTID ));

	if ( m_iWordsCheckpoint!=RTDICT_CHECKPOINT_V3 && m_iWordsCheckpoint!=RTDICT_CHECKPOINT_V5 )
		LOC_FAIL(( fp, "unexpected number of words per checkpoint (expected 1024 or 48, got %d)", m_iWordsCheckpoint ));

	ARRAY_FOREACH ( iSegment, m_dRamChunks )
	{
		fprintf ( fp, "checking RT segment %d(%d)...\n", iSegment, m_dRamChunks.GetLength() );

		if ( !m_dRamChunks[iSegment] )
		{
			LOC_FAIL(( fp, "missing RT segment (segment=%d)", iSegment ));
			continue;
		}

		RtSegment_t & tSegment = *m_dRamChunks[iSegment];
		if ( tSegment.m_bTlsKlist )
			LOC_FAIL(( fp, "TLS k-list flag on: index is being commited (segment=%d)", iSegment ));

		if ( !tSegment.m_iRows )
		{
			LOC_FAIL(( fp, "empty RT segment (segment=%d)", iSegment ));
			continue;
		}

		const BYTE * pCurWord = tSegment.m_dWords.Begin();
		const BYTE * pMaxWord = pCurWord+tSegment.m_dWords.GetLength();
		const BYTE * pCurDoc = tSegment.m_dDocs.Begin();
		const BYTE * pMaxDoc = pCurDoc+tSegment.m_dDocs.GetLength();
		const BYTE * pCurHit = tSegment.m_dHits.Begin();
		const BYTE * pMaxHit = pCurHit+tSegment.m_dHits.GetLength();

		CSphVector<RtWordCheckpoint_t> dRefCheckpoints;
		int nWordsRead = 0;
		int nCheckpointWords = 0;
		int iCheckpointOffset = 0;
		SphWordID_t uPrevWordID = 0;
		DWORD uPrevDocOffset = 0;
		DWORD uPrevHitOffset = 0;

		CSphVector<SphDocID_t> dUsedKListEntries;

		RtWord_t tWord;
		memset ( &tWord, 0, sizeof(tWord) );

		BYTE sWord[SPH_MAX_KEYWORD_LEN+2], sLastWord[SPH_MAX_KEYWORD_LEN+2];
		memset ( sWord, 0, sizeof(sWord) );
		memset ( sLastWord, 0, sizeof(sLastWord) );

		int iLastWordLen = 0, iWordLen = 0;

		while ( pCurWord && pCurWord<pMaxWord )
		{
			bool bCheckpoint = ++nCheckpointWords==m_iWordsCheckpoint;
			if ( bCheckpoint )
			{
				nCheckpointWords = 1;
				iCheckpointOffset = pCurWord - tSegment.m_dWords.Begin();
				tWord.m_uDoc = 0;
				if ( !m_bKeywordDict )
					tWord.m_uWordID = 0;
			}

			const BYTE * pIn = pCurWord;
			DWORD uDeltaDoc;
			if ( m_bKeywordDict )
			{
				BYTE iMatch, iDelta, uPacked;
				uPacked = *pIn++;

				if ( pIn>=pMaxWord )
				{
					LOC_FAIL(( fp, "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead ));
					break;
				}

				if ( uPacked & 0x80 )
				{
					iDelta = ( ( uPacked>>4 ) & 7 ) + 1;
					iMatch = uPacked & 15;
				} else
				{
					iDelta = uPacked & 127;
					iMatch = *pIn++;
					if ( pIn>=pMaxWord )
					{
						LOC_FAIL(( fp, "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead ));
						break;
					}

					if ( iDelta<=8 && iMatch<=15 )
					{
						sLastWord[sizeof(sLastWord)-1] = '\0';
						LOC_FAIL(( fp, "wrong word-delta (segment=%d, word=%d, last_word=%s, last_len=%d, match=%d, delta=%d)",
							iSegment, nWordsRead, sLastWord+1, iLastWordLen, iMatch, iDelta ));
					}
				}

				if ( iMatch+iDelta>=(int)sizeof(sWord)-2 || iMatch>iLastWordLen )
				{
					sLastWord[sizeof(sLastWord)-1] = '\0';
					LOC_FAIL(( fp, "wrong word-delta (segment=%d, word=%d, last_word=%s, last_len=%d, match=%d, delta=%d)",
						iSegment, nWordsRead, sLastWord+1, iLastWordLen, iMatch, iDelta ));

					pIn += iDelta;
					if ( pIn>=pMaxWord )
					{
						LOC_FAIL(( fp, "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead ));
						break;
					}
				} else
				{
					iWordLen = iMatch+iDelta;
					sWord[0] = (BYTE)iWordLen;
					memcpy ( sWord+1+iMatch, pIn, iDelta );
					sWord[1+iWordLen] = 0;
					pIn += iDelta;
					if ( pIn>=pMaxWord )
					{
						LOC_FAIL(( fp, "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead ));
						break;
					}
				}

				int iCalcWordLen = strlen ( (const char *)sWord+1 );
				if ( iWordLen!=iCalcWordLen )
				{
					sWord[sizeof(sWord)-1] = '\0';
					LOC_FAIL(( fp, "word length mismatch (segment=%d, word=%d, read_word=%s, read_len=%d, calc_len=%d)",
						iSegment, nWordsRead, sWord+1, iWordLen, iCalcWordLen ));
				}

				if ( !iWordLen )
					LOC_FAIL(( fp, "empty word in word list (segment=%d, word=%d)",	iSegment, nWordsRead ));

				const BYTE * pStr = sWord+1;
				const BYTE * pStringStart = pStr;
				while ( pStringStart-pStr < iWordLen )
				{
					if ( !*pStringStart )
					{
						CSphString sErrorStr;
						sErrorStr.SetBinary ( (const char*)pStr, iWordLen );
						LOC_FAIL(( fp, "embedded zero in a word list string (segment=%d, offset=%u, string=%s)",
							iSegment, (DWORD)(pStringStart-pStr), sErrorStr.cstr() ));
					}

					pStringStart++;
				}

				if ( iLastWordLen && iWordLen )
				{
					if ( sphDictCmpStrictly ( (const char *)sWord+1, iWordLen, (const char *)sLastWord+1, iLastWordLen )<=0 )
					{
						sWord[sizeof(sWord)-1] = '\0';
						sLastWord[sizeof(sLastWord)-1] = '\0';
						LOC_FAIL(( fp, "word order decreased (segment=%d, word=%d, read_word=%s, last_word=%s)",
							iSegment, nWordsRead, sWord+1, sLastWord+1 ));
					}
				}

				memcpy ( sLastWord, sWord, iWordLen+2 );
				iLastWordLen = iWordLen;
			} else
			{
				SphWordID_t uDeltaID;
				pIn = UnzipWordid ( &uDeltaID, pIn );
				if ( pIn>=pMaxWord )
					LOC_FAIL(( fp, "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead ));

				tWord.m_uWordID += uDeltaID;

				if ( tWord.m_uWordID<=uPrevWordID )
				{
					LOC_FAIL(( fp, "wordid decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", previd=" UINT64_FMT ")",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)uPrevWordID ));
				}

				uPrevWordID = tWord.m_uWordID;
			}

			pIn = UnzipDword ( &tWord.m_uDocs, pIn );
			if ( pIn>=pMaxWord )
			{
				sWord[sizeof(sWord)-1] = '\0';
				LOC_FAIL(( fp, "invalid docs/hits (segment=%d, word=%d, read_word=%s, docs=%u, hits=%u)", iSegment, nWordsRead,
					sWord+1, tWord.m_uDocs, tWord.m_uHits ));
			}

			pIn = UnzipDword ( &tWord.m_uHits, pIn );
			if ( pIn>=pMaxWord )
				LOC_FAIL(( fp, "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead ));

			pIn = UnzipDword ( &uDeltaDoc, pIn );
			if ( pIn>pMaxWord )
				LOC_FAIL(( fp, "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead ));

			pCurWord = pIn;
			tWord.m_uDoc += uDeltaDoc;

			if ( !tWord.m_uDocs || !tWord.m_uHits || tWord.m_uHits<tWord.m_uDocs )
			{
				sWord[sizeof(sWord)-1] = '\0';
				LOC_FAIL(( fp, "invalid docs/hits (segment=%d, word=%d, read_wordid=" UINT64_FMT
					", read_word=%s, docs=%u, hits=%u)",
					iSegment, nWordsRead, (uint64_t)tWord.m_uWordID,
					sWord+1, tWord.m_uDocs, tWord.m_uHits ));
			}

			if ( bCheckpoint )
			{
				RtWordCheckpoint_t & tCP = dRefCheckpoints.Add();
				tCP.m_iOffset = iCheckpointOffset;

				if ( m_bKeywordDict )
				{
					tCP.m_sWord = new char [sWord[0]+1];
					memcpy ( (void *)tCP.m_sWord, sWord+1, sWord[0]+1 );
				} else
					tCP.m_uWordID = tWord.m_uWordID;
			}

			sWord[sizeof(sWord)-1] = '\0';

			if ( uPrevDocOffset && tWord.m_uDoc<=uPrevDocOffset )
				LOC_FAIL(( fp, "doclist offset decreased (segment=%d, word=%d, "
					"read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, prev_doclist_offset=%u)",
					iSegment, nWordsRead,
					(uint64_t)tWord.m_uWordID, sWord+1, tWord.m_uDoc, uPrevDocOffset ));

			// read doclist
			DWORD uDocOffset = pCurDoc-tSegment.m_dDocs.Begin();
			if ( tWord.m_uDoc!=uDocOffset )
			{
				LOC_FAIL(( fp, "unexpected doclist offset (wordid=" UINT64_FMT "(%s)(%d), "
					"doclist_offset=%u, expected_offset=%u)",
					(uint64_t)tWord.m_uWordID, sWord+1, nWordsRead,
					tWord.m_uDoc, uDocOffset ));

				if ( uDocOffset>=(DWORD)tSegment.m_dDocs.GetLength() )
				{
					LOC_FAIL(( fp, "doclist offset pointing past doclist (segment=%d, word=%d, "
						"read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead,
						sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() ));

					nWordsRead++;
					continue;
				} else
					pCurDoc = tSegment.m_dDocs.Begin()+uDocOffset;
			}

			// read all docs from doclist
			RtDoc_t tDoc;
			memset ( &tDoc, 0, sizeof(tDoc) );
			SphDocID_t uPrevDocID = 0;

			for ( DWORD uDoc=0; uDoc<tWord.m_uDocs && pCurDoc<pMaxDoc; uDoc++ )
			{
				bool bEmbeddedHit = false;
				pIn = pCurDoc;
				SphDocID_t uDeltaID;
				pIn = UnzipDocid ( &uDeltaID, pIn );

				if ( pIn>=pMaxDoc )
				{
					LOC_FAIL(( fp, "reading past doclist end (segment=%d, word=%d, "
						"read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead,
						(uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() ));
					break;
				}

				tDoc.m_uDocID += uDeltaID;
				DWORD uDocField;
				pIn = UnzipDword ( &uDocField, pIn );
				if ( pIn>=pMaxDoc )
				{
					LOC_FAIL(( fp, "reading past doclist end (segment=%d, word=%d, "
						"read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead,
						(uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() ));
					break;
				}

				tDoc.m_uDocFields = uDocField;
				pIn = UnzipDword ( &tDoc.m_uHits, pIn );
				if ( pIn>=pMaxDoc )
				{
					LOC_FAIL(( fp, "reading past doclist end (segment=%d, word=%d, "
						"read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead,
						(uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() ));
					break;
				}

				if ( tDoc.m_uHits==1 )
				{
					bEmbeddedHit = true;

					DWORD a, b;
					pIn = UnzipDword ( &a, pIn );
					if ( pIn>=pMaxDoc )
					{
						LOC_FAIL(( fp, "reading past doclist end (segment=%d, word=%d, "
							"read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead,
							(uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() ));
						break;
					}

					pIn = UnzipDword ( &b, pIn );
					if ( pIn>pMaxDoc )
					{
						LOC_FAIL(( fp, "reading past doclist end (segment=%d, word=%d, "
							"read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead,
							(uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() ));
						break;
					}

					tDoc.m_uHit = HITMAN::Create ( b, a );
				} else
				{
					pIn = UnzipDword ( &tDoc.m_uHit, pIn );
					if ( pIn>pMaxDoc )
					{
						LOC_FAIL(( fp, "reading past doclist end (segment=%d, word=%d, "
							"read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead,
							(uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() ));
						break;
					}
				}
				pCurDoc = pIn;

				if ( tDoc.m_uDocID<=uPrevDocID )
				{
					LOC_FAIL(( fp, "docid decreased (segment=%d, word=%d, "
						"read_wordid=" UINT64_FMT ", read_word=%s, docid=" UINT64_FMT ", prev_docid=" UINT64_FMT ")",
						iSegment, nWordsRead,
						(uint64_t)tWord.m_uWordID, sWord+1, (uint64_t)tDoc.m_uDocID, (uint64_t)uPrevDocID ));
				}

				if ( !tSegment.FindRow ( tDoc.m_uDocID ) )
					LOC_FAIL(( fp, "no attributes found (segment=%d, word=%d, "
						"wordid=" UINT64_FMT ", docid=" UINT64_FMT ")",
						iSegment, nWordsRead,
						(uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID ));

				if ( bEmbeddedHit )
				{
					DWORD uFieldId = HITMAN::GetField ( tDoc.m_uHit );
					DWORD uFieldMask = tDoc.m_uDocFields;
					int iCounter = 0;
					for ( ; uFieldMask; iCounter++ )
						uFieldMask &= uFieldMask - 1;

					if ( iCounter!=1 || tDoc.m_uHits!=1 )
					{
						LOC_FAIL(( fp, "embedded hit with multiple occurences in a document found "
							"(segment=%d, word=%d, wordid=" UINT64_FMT ", docid=" UINT64_FMT ")",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID ));
					}

					if ( (int)uFieldId>m_tSchema.GetFieldsCount() || uFieldId>SPH_MAX_FIELDS )
					{
						LOC_FAIL(( fp, "invalid field id in an embedded hit (segment=%d, word=%d, "
							"wordid=" UINT64_FMT ", docid=" UINT64_FMT ", field_id=%u, total_fields=%d)",
							iSegment, nWordsRead,
							(uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, uFieldId, m_tSchema.GetFieldsCount() ));
					}

					if ( !( tDoc.m_uDocFields & ( 1 << uFieldId ) ) )
					{
						LOC_FAIL(( fp, "invalid field id: not in doclist mask (segment=%d, word=%d, "
							"wordid=" UINT64_FMT ", docid=" UINT64_FMT ", field_id=%u, field_mask=%u)",
							iSegment, nWordsRead,
							(uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, uFieldId, tDoc.m_uDocFields ));
					}
				} else
				{
					DWORD uExpectedHitOffset = pCurHit-tSegment.m_dHits.Begin();
					if ( tDoc.m_uHit!=uExpectedHitOffset )
					{
						LOC_FAIL(( fp, "unexpected hitlist offset (segment=%d, word=%d, "
							"wordid=" UINT64_FMT ", docid=" UINT64_FMT ", offset=%u, expected_offset=%u",
							iSegment, nWordsRead,
							(uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, tDoc.m_uHit, uExpectedHitOffset ));
					}

					if ( tDoc.m_uHit && tDoc.m_uHit<=uPrevHitOffset )
					{
						LOC_FAIL(( fp, "hitlist offset decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", docid=" UINT64_FMT ", offset=%u, prev_offset=%u",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, tDoc.m_uHit, uPrevHitOffset ));
					}

					// check hitlist
					DWORD uHitlistEntry = 0;
					DWORD uLastPosInField = 0;
					DWORD uLastFieldId = 0;
					bool bLastInFieldFound = false;

					for ( DWORD uHit = 0; uHit < tDoc.m_uHits && pCurHit; uHit++ )
					{
						DWORD uValue = 0;
						pCurHit = UnzipDword ( &uValue, pCurHit );
						if ( pCurHit>pMaxHit )
						{
							LOC_FAIL(( fp, "reading past hitlist end (segment=%d, word=%d, wordid=" UINT64_FMT ", docid=" UINT64_FMT ")",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID ));
							break;
						}

						uHitlistEntry += uValue;

						DWORD uPosInField = HITMAN::GetPos ( uHitlistEntry );
						bool bLastInField = HITMAN::IsEnd ( uHitlistEntry );
						DWORD uFieldId = HITMAN::GetField ( uHitlistEntry );

						if ( (int)uFieldId>m_tSchema.GetFieldsCount() || uFieldId>SPH_MAX_FIELDS )
						{
							LOC_FAIL(( fp, "invalid field id in a hitlist (segment=%d, word=%d, "
								"wordid=" UINT64_FMT ", docid=" UINT64_FMT ", field_id=%u, total_fields=%d)",
								iSegment, nWordsRead,
								(uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, uFieldId, m_tSchema.GetFieldsCount() ));
						}

						if ( !( tDoc.m_uDocFields & ( 1 << uFieldId ) ) )
						{
							LOC_FAIL(( fp, "invalid field id: not in doclist mask (segment=%d, word=%d, "
								"wordid=" UINT64_FMT ", docid=" UINT64_FMT ", field_id=%u, field_mask=%u)",
								iSegment, nWordsRead,
								(uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, uFieldId, tDoc.m_uDocFields ));
						}

						if ( uLastFieldId!=uFieldId )
						{
							bLastInFieldFound = false;
							uLastPosInField = 0;
						}

						if ( uLastPosInField && uPosInField<=uLastPosInField )
						{
							LOC_FAIL(( fp, "hit position in field decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", docid=" UINT64_FMT ", pos=%u, last_pos=%u)",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, uPosInField, uLastPosInField ));
						}

						if ( bLastInField && bLastInFieldFound )
						{
							LOC_FAIL(( fp, "duplicate last-in-field hit found (segment=%d, word=%d, wordid=" UINT64_FMT ", docid=" UINT64_FMT ")",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID ));
						}

						uLastPosInField = uPosInField;
						uLastFieldId = uFieldId;
						bLastInFieldFound |= bLastInField;
					}

					uPrevHitOffset = tDoc.m_uHit;
				}

				DWORD uAvailFieldMask = ( 1 << m_tSchema.GetFieldsCount() ) - 1;
				if ( tDoc.m_uDocFields & ~uAvailFieldMask )
				{
					LOC_FAIL(( fp, "wrong document field mask (segment=%d, word=%d, wordid=" UINT64_FMT ", docid=" UINT64_FMT ", mask=%u, total_fields=%d",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)tDoc.m_uDocID, tDoc.m_uDocFields, m_tSchema.GetFieldsCount() ));
				}

				if ( tSegment.GetKlist().BinarySearch ( tDoc.m_uDocID ) )
					dUsedKListEntries.Add ( tDoc.m_uDocID );

				uPrevDocID = tDoc.m_uDocID;
			}

			uPrevDocOffset = tWord.m_uDoc;
			nWordsRead++;
		}

		if ( pCurDoc!=pMaxDoc )
			LOC_FAIL(( fp, "unused doclist entries found (segment=%d, doclist_size=%d)",
				iSegment, tSegment.m_dDocs.GetLength() ));

		if ( pCurHit!=pMaxHit )
			LOC_FAIL(( fp, "unused hitlist entries found (segment=%d, hitlist_size=%d)",
				iSegment, tSegment.m_dHits.GetLength() ));

		if ( dRefCheckpoints.GetLength()!=tSegment.m_dWordCheckpoints.GetLength() )
			LOC_FAIL(( fp, "word checkpoint count mismatch (read=%d, calc=%d)",
				tSegment.m_dWordCheckpoints.GetLength(), dRefCheckpoints.GetLength() ));

		for ( int i=0; i < Min ( dRefCheckpoints.GetLength(), tSegment.m_dWordCheckpoints.GetLength() ); i++ )
		{
			const RtWordCheckpoint_t & tRefCP = dRefCheckpoints[i];
			const RtWordCheckpoint_t & tCP = tSegment.m_dWordCheckpoints[i];
			const int iLen = m_bKeywordDict ? strlen ( tCP.m_sWord ) : 0;
			if ( m_bKeywordDict && ( !tCP.m_sWord || ( !strlen ( tRefCP.m_sWord ) || !strlen ( tCP.m_sWord ) ) ) )
			{
				LOC_FAIL(( fp, "empty word checkpoint %d ((segment=%d, read_word=%s, read_len=%u, readpos=%d, calc_word=%s, calc_len=%u, calcpos=%d)",
					i, iSegment, tCP.m_sWord, (DWORD)strlen ( tCP.m_sWord ), tCP.m_iOffset,
					tRefCP.m_sWord, (DWORD)strlen ( tRefCP.m_sWord ), tRefCP.m_iOffset ));
			} else if ( sphCheckpointCmpStrictly ( tCP.m_sWord, iLen, tCP.m_uWordID, m_bKeywordDict, tRefCP ) || tRefCP.m_iOffset!=tCP.m_iOffset )
			{
				if ( m_bKeywordDict )
				{
					LOC_FAIL(( fp, "word checkpoint %d differs (segment=%d, read_word=%s, readpos=%d, calc_word=%s, calcpos=%d)",
						i, iSegment, tCP.m_sWord, tCP.m_iOffset, tRefCP.m_sWord, tRefCP.m_iOffset ));
				} else
				{
					LOC_FAIL(( fp, "word checkpoint %d differs (segment=%d, readid=" UINT64_FMT ", readpos=%d, calcid=" UINT64_FMT ", calcpos=%d)",
						i, iSegment, (uint64_t)tCP.m_uWordID, tCP.m_iOffset, (int64_t)tRefCP.m_uWordID, tRefCP.m_iOffset ));
				}
			}
		}

		if ( m_bKeywordDict )
			ARRAY_FOREACH ( i, dRefCheckpoints )
				SafeDeleteArray ( dRefCheckpoints[i].m_sWord );

		dRefCheckpoints.Reset ();

		// check killlists
		dUsedKListEntries.Uniq();
		int nUsedKListEntries = dUsedKListEntries.GetLength();

		if ( nUsedKListEntries!=tSegment.GetKlist().GetLength() )
		{
			LOC_FAIL(( fp, "used killlist entries mismatch (segment=%d, klist_entries=%d, used_entries=%d)",
				iSegment, tSegment.GetKlist().GetLength(), nUsedKListEntries ));
		}

		// check attributes
		if ( tSegment.m_iRows*m_iStride!=tSegment.m_dRows.GetLength() )
		{
			LOC_FAIL(( fp, "rowitems count mismatch (segment=%d, expected=%d, current=%d)",
				iSegment, tSegment.m_iRows*m_iStride, tSegment.m_dRows.GetLength() ));
		}

		CSphVector<int> dMvaItems;
		CSphVector<CSphAttrLocator> dFloatItems;
		CSphVector<CSphAttrLocator> dStrItems;
		CSphVector<CSphAttrLocator> dJsonItems;
		for ( int iAttr=0; iAttr<m_tSchema.GetAttrsCount(); iAttr++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(iAttr);
			if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET )
			{
				if ( tAttr.m_tLocator.m_iBitCount!=ROWITEM_BITS )
				{
					LOC_FAIL(( fp, "unexpected MVA bitcount (segment=%d, attr=%d, expected=%d, got=%d)",
						iSegment, iAttr, ROWITEM_BITS, tAttr.m_tLocator.m_iBitCount ));
					continue;
				}
				if ( ( tAttr.m_tLocator.m_iBitOffset % ROWITEM_BITS )!=0 )
				{
					LOC_FAIL(( fp, "unaligned MVA bitoffset (segment=%d, attr=%d, bitoffset=%d)",
						iSegment, iAttr, tAttr.m_tLocator.m_iBitOffset ));
					continue;
				}
				if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
					dMvaItems.Add ( tAttr.m_tLocator.m_iBitOffset/ROWITEM_BITS );
			} else if ( tAttr.m_eAttrType==SPH_ATTR_FLOAT )
				dFloatItems.Add	( tAttr.m_tLocator );
			else if ( tAttr.m_eAttrType==SPH_ATTR_STRING )
				dStrItems.Add ( tAttr.m_tLocator );
			else if ( tAttr.m_eAttrType==SPH_ATTR_JSON )
				dJsonItems.Add ( tAttr.m_tLocator );
		}
		int iMva64 = dMvaItems.GetLength();
		for ( int iAttr=0; iAttr<m_tSchema.GetAttrsCount(); iAttr++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(iAttr);
			if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
				dMvaItems.Add ( tAttr.m_tLocator.m_iBitOffset/ROWITEM_BITS );
		}

		CSphVector<DWORD> dStringOffsets;
		if ( tSegment.m_dStrings.GetLength() > 1 )
		{
			const BYTE * pBaseStr = tSegment.m_dStrings.Begin();
			const BYTE * pCurStr = pBaseStr + 1;
			const BYTE * pMaxStr = pBaseStr + tSegment.m_dStrings.GetLength();
			while ( pCurStr<pMaxStr )
			{
				const BYTE * pStr = NULL;
				const int iLen = sphUnpackStr ( pCurStr, &pStr );

				if ( !iLen )
					LOC_FAIL(( fp, "empty attribute string found (segment=%d, offset=%u)", iSegment, (DWORD)(pCurStr-pBaseStr) ));

				if ( pStr+iLen>pMaxStr || pStr<pCurStr || pStr>pCurStr+4 )
				{
					LOC_FAIL(( fp, "string length out of bounds (segment=%d, offset=%u, len=%d)", iSegment, (DWORD)(pCurStr-pBaseStr), iLen ));
					break;
				}

				dStringOffsets.Add ( (DWORD)(pCurStr-pBaseStr) );

				pCurStr = pStr + iLen;
			}
		}

		// loop the rows
		const CSphRowitem * pRow = tSegment.m_dRows.Begin();
		const CSphRowitem * pRowMax = pRow + tSegment.m_dRows.GetLength();
		const DWORD * pMvaBase = tSegment.m_dMvas.Begin();
		const DWORD * pMvaMax = pMvaBase + tSegment.m_dMvas.GetLength();
		const DWORD * pMvaCur = pMvaBase + 1;

		SphDocID_t uLastID = 0;
		DWORD uLastStrOffset = 0;
		int nCalcAliveRows = 0;
		int nCalcRows = 0;
		int nUsedStrings = 0;
		int nUsedJsons = 0;

		for ( DWORD uRow=0; pRow<pRowMax; uRow++, pRow+=m_iStride )
		{
			if ( uLastID>=DOCINFO2ID(pRow) )
				LOC_FAIL(( fp, "docid decreased in attributes (segment=%d, row=%u, docid=" DOCID_FMT ", lastid=" DOCID_FMT ")",
					iSegment, uRow, DOCINFO2ID(pRow), uLastID ));

			uLastID = DOCINFO2ID(pRow);

			///////////////////////////
			// check MVAs
			///////////////////////////

			if ( dMvaItems.GetLength() )
			{
				const CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);

				ARRAY_FOREACH ( iItem, dMvaItems )
				{
					const DWORD uOffset = pAttrs[dMvaItems[iItem]];
					if ( !uOffset )
						continue;

					if ( pMvaBase+uOffset>=pMvaMax )
					{
						LOC_FAIL(( fp, "MVA index out of bounds (segment=%d, row=%u, mvaattr=%d, docid=" DOCID_FMT ", index=%u)",
							iSegment, uRow, iItem, uLastID, uOffset ));
						continue;
					}

					if ( pMvaCur!=pMvaBase+uOffset )
					{
						LOC_FAIL(( fp, "wrong MVA offset (segment=%d, row=%u, mvaattr=%d, docid=" DOCID_FMT ", expected=%u, got=%u)",
							iSegment, uRow, iItem, uLastID, (DWORD)(pMvaCur-pMvaBase), uOffset ));

						pMvaCur = pMvaBase+uOffset;
					}

					// check values
					DWORD uValues = *pMvaCur++;

					if ( pMvaCur+uValues-1>=pMvaMax )
					{
						LOC_FAIL(( fp, "MVA count out of bounds (segment=%d, row=%u, mvaattr=%d, docid=" DOCID_FMT ", count=%u)",
							iSegment, uRow, iItem, uLastID, uValues ));
						pMvaCur += uValues;
						continue;
					}

					// check that values are ascending
					for ( DWORD uVal=(iItem>=iMva64 ? 2 : 1); uVal<uValues; )
					{
						int64_t iPrev, iCur;
						if ( iItem>=iMva64 )
						{
							iPrev = MVA_UPSIZE ( pMvaCur+uVal-2 );
							iCur = MVA_UPSIZE ( pMvaCur+uVal );
							uVal += 2;
						} else
						{
							iPrev = pMvaCur[uVal-1];
							iCur = pMvaCur[uVal];
							uVal++;
						}

						if ( iCur<=iPrev )
						{
							LOC_FAIL(( fp, "unsorted MVA values (segment=%d, row=%u, mvaattr=%d, docid=" DOCID_FMT ", val[%u]=" INT64_FMT ", val[%u]=" INT64_FMT ")",
								iSegment, uRow, iItem, uLastID, ( iItem>=iMva64 ? uVal-2 : uVal-1 ), iPrev, uVal, iCur ));
						}

						uVal += ( iItem>=iMva64 ? 2 : 1 );
					}

					pMvaCur += uValues;
				}
			}

			///////////////////////////
			// check floats
			///////////////////////////

			ARRAY_FOREACH ( iItem, dFloatItems )
			{
				const CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);
				const DWORD uValue = (DWORD)sphGetRowAttr ( pAttrs, dFloatItems[iItem] );
				const DWORD uExp = ( uValue >> 23 ) & 0xff;
				const DWORD uMantissa = uValue & 0x003fffff;

				// check normalized
				if ( uExp==0 && uMantissa!=0 )
					LOC_FAIL(( fp, "float attribute value is unnormalized (segment=%d, row=%u, attr=%d, id=" DOCID_FMT ", raw=0x%x, value=%f)",
						iSegment, uRow, iItem, uLastID, uValue, sphDW2F ( uValue ) ));

				// check +-inf
				if ( uExp==0xff && uMantissa==0 )
					LOC_FAIL(( fp, "float attribute is infinity (segment=%d, row=%u, attr=%d, id=" DOCID_FMT ", raw=0x%x, value=%f)",
						iSegment, uRow, iItem, uLastID, uValue, sphDW2F ( uValue ) ));
			}

			/////////////////
			// check strings
			/////////////////

			ARRAY_FOREACH ( iItem, dStrItems )
			{
				const CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);

				const DWORD uOffset = (DWORD)sphGetRowAttr ( pAttrs, dStrItems[iItem] );
				if ( uOffset>=(DWORD)tSegment.m_dStrings.GetLength() )
				{
					LOC_FAIL(( fp, "string offset out of bounds (segment=%d, row=%u, stringattr=%d, docid=" DOCID_FMT ", index=%u)",
						iSegment, uRow, iItem, uLastID, uOffset ));
					continue;
				}

				if ( !uOffset )
					continue;

				bool bLastOff4UpdatedJson = ( iItem==0 && dJsonItems.GetLength () );
				if ( uLastStrOffset>=uOffset && !bLastOff4UpdatedJson )
					LOC_FAIL(( fp, "string offset decreased (segment=%d, row=%u, stringattr=%d, docid=" DOCID_FMT ", offset=%u, last_offset=%u)",
						iSegment, uRow, iItem, uLastID, uOffset, uLastStrOffset ));

				if ( !dStringOffsets.BinarySearch ( uOffset ) )
				{
					LOC_FAIL(( fp, "string offset is not a string start (segment=%d, row=%u, stringattr=%d, docid=" DOCID_FMT ", offset=%u)",
						iSegment, uRow, iItem, uLastID, uOffset ));
				} else
					nUsedStrings++;

				const BYTE * pStr = NULL;
				int iLen = sphUnpackStr ( tSegment.m_dStrings.Begin()+uOffset, &pStr );
				const BYTE * pStringStart = pStr;
				while ( pStringStart-pStr < iLen )
				{
					if ( !*pStringStart )
					{
						CSphString sErrorStr;
						sErrorStr.SetBinary ( (const char*)pStr, iLen );
						LOC_FAIL(( fp, "embedded zero in a string (segment=%d, offset=%u, string=%s)",
									iSegment, uOffset, sErrorStr.cstr() ));
					}

					pStringStart++;
				}

				uLastStrOffset = uOffset;
			}

			/////////////////////////////
			// check JSON attributes
			/////////////////////////////

			ARRAY_FOREACH ( iItem, dJsonItems )
			{
				const CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);

				const DWORD uOffset = (DWORD)sphGetRowAttr ( pAttrs, dJsonItems[iItem] );
				if ( uOffset>=(DWORD)tSegment.m_dStrings.GetLength() )
				{
					LOC_FAIL(( fp, "string(JSON) offset out of bounds (segment=%d, row=%u, stringattr=%d, docid=" DOCID_FMT ", index=%u)",
						iSegment, uRow, iItem, uLastID, uOffset ));
					continue;
				}

				if ( !uOffset )
					continue;

				if ( uLastStrOffset>=uOffset )
					LOC_FAIL(( fp, "string(JSON) offset decreased (segment=%d, row=%u, stringattr=%d, docid=" DOCID_FMT ", offset=%u, last_offset=%u)",
						iSegment, uRow, iItem, uLastID, uOffset, uLastStrOffset ));

				if ( !dStringOffsets.BinarySearch ( uOffset ) )
				{
					LOC_FAIL(( fp, "string(JSON) offset is not a string start (segment=%d, row=%u, stringattr=%d, docid=" DOCID_FMT ", offset=%u)",
						iSegment, uRow, iItem, uLastID, uOffset ));
				} else
					nUsedJsons++;

				const BYTE * pData = NULL;
				int iBlobLen = sphUnpackStr ( tSegment.m_dStrings.Begin()+uOffset, &pData );

				const BYTE * p = pData+4;
				CSphVector<ESphJsonType> dStateStack;
				if ( pData[0] | pData[1]<<8 | pData[2]<<16 | pData[3]<<24 )
					dStateStack.Add ( JSON_OBJECT );

				do
				{
					ESphJsonType eType = (ESphJsonType)*p++;

					if ( dStateStack.GetLength() && dStateStack.Last()==JSON_OBJECT && eType!=JSON_EOF )
					{
						int iKeyLen = sphJsonUnpackInt ( &p );
						p += iKeyLen;
					}

					if ( dStateStack.GetLength() && dStateStack.Last()==JSON_MIXED_VECTOR )
						dStateStack.Pop();

					switch ( eType )
					{
					case JSON_EOF:
					{
						if ( dStateStack.GetLength() && dStateStack.Last()==JSON_OBJECT )
							dStateStack.Pop();
						break;
					}

					case JSON_INT32:		sphJsonLoadInt ( &p ); break;
					case JSON_INT64:		sphJsonLoadBigint ( &p ); break;
					case JSON_DOUBLE:		sphJsonLoadBigint ( &p ); break;
					case JSON_TRUE:			break;
					case JSON_FALSE:		break;
					case JSON_NULL:			break;

					case JSON_STRING:
					{
						int iStrLen = sphJsonUnpackInt ( &p );
						p += iStrLen;
						break;
					}

					case JSON_OBJECT:
					{
						dStateStack.Add ( JSON_OBJECT );
						sphJsonUnpackInt ( &p );
						p += 4; // bloom mask
						break;
					}

					case JSON_MIXED_VECTOR:
					{
						sphJsonUnpackInt ( &p );
						for ( int iLen=sphJsonUnpackInt ( &p ); iLen; iLen-- )
							dStateStack.Add ( JSON_MIXED_VECTOR );
						break;
					}

					case JSON_STRING_VECTOR:
					{
						int iTotalLen = sphJsonUnpackInt ( &p );
						p += iTotalLen;
						break;
					}

					case JSON_INT32_VECTOR:
					{
						for ( int iLen=sphJsonUnpackInt ( &p ); iLen; iLen-- )
							sphJsonLoadInt ( &p );
						break;
					}

					case JSON_INT64_VECTOR:
					case JSON_DOUBLE_VECTOR:
					{
						for ( int iLen=sphJsonUnpackInt ( &p ); iLen; iLen-- )
							sphJsonLoadBigint ( &p );
						break;
					}

					default:
						LOC_FAIL(( fp, "incorrect type in JSON blob (type=%d", eType ));
						break;
					}
				} while ( p<( pData+iBlobLen ));

				if ( dStateStack.GetLength() )
					LOC_FAIL(( fp, "JSON blob nested arrays/objects mismatch"));

				if ( iBlobLen!=( p-pData ))
					LOC_FAIL(( fp, "JSON blob length mismatch (stored=%d, actual=%d)", iBlobLen, int( p-pData ) ));

				uLastStrOffset = uOffset;
			}

			nCalcRows++;
			if ( !tSegment.GetKlist().BinarySearch ( uLastID ) )
				nCalcAliveRows++;
		}

		if ( ( nUsedStrings+nUsedJsons )!=dStringOffsets.GetLength() )
			LOC_FAIL(( fp, "unused string/JSON entries found (segment=%d)", iSegment ));

		if ( dMvaItems.GetLength() && pMvaCur!=pMvaMax )
			LOC_FAIL(( fp, "unused MVA entries found (segment=%d)", iSegment ));

		if ( tSegment.m_iRows!=nCalcRows )
			LOC_FAIL(( fp, "row count mismatch (segment=%d, expected=%d, current=%d)",
				iSegment, nCalcRows, tSegment.m_iRows ));

		if ( tSegment.m_iAliveRows!=nCalcAliveRows )
			LOC_FAIL(( fp, "alive row count mismatch (segment=%d, expected=%d, current=%d)",
				iSegment, nCalcAliveRows, tSegment.m_iAliveRows ));
	}

	ARRAY_FOREACH ( i, m_dDiskChunks )
	{
		fprintf ( fp, "checking disk chunk %d(%d)...\n", i, m_dDiskChunks.GetLength() );
		iFailsPlain += m_dDiskChunks[i]->DebugCheck ( fp );
	}

	tmCheck = sphMicroTimer() - tmCheck;
	if ( ( iFails+iFailsPlain )==0 )
		fprintf ( fp, "check passed" );
	else if ( iFails!=iFailsPrinted )
		fprintf ( fp, "check FAILED, %d of %d failures reported", iFailsPrinted, iFails+iFailsPlain );
	else
		fprintf ( fp, "check FAILED, %d failures reported", iFails+iFailsPlain );

	fprintf ( fp, ", %d.%d sec elapsed\n", (int)(tmCheck/1000000), (int)((tmCheck/100000)%10) );

	return iFails + iFailsPlain;
} // NOLINT function length

//////////////////////////////////////////////////////////////////////////
// SEARCHING
//////////////////////////////////////////////////////////////////////////


struct RtQwordTraits_t : public ISphQword
{
public:
	virtual bool Setup ( const RtIndex_t * pIndex, int iSegment, const SphChunkGuard_t & tGuard ) = 0;
};


//////////////////////////////////////////////////////////////////////////

struct RtQword_t : public RtQwordTraits_t
{
public:
	RtQword_t ()
		: m_uNextHit ( 0 )
		, m_pKill ( NULL )
		, m_pKillEnd ( NULL )
	{
		m_tMatch.Reset ( 0 );
	}

	virtual ~RtQword_t ()
	{
	}

	virtual const CSphMatch & GetNextDoc ( DWORD * )
	{
		while (true)
		{
			const RtDoc_t * pDoc = m_tDocReader.UnzipDoc();
			if ( !pDoc )
			{
				m_tMatch.m_uDocID = 0;
				return m_tMatch;
			}

			if ( sphBinarySearch ( m_pKill, m_pKillEnd, pDoc->m_uDocID )!=NULL )
				continue;

			m_tMatch.m_uDocID = pDoc->m_uDocID;
			m_dQwordFields.Assign32 ( pDoc->m_uDocFields );
			m_uMatchHits = pDoc->m_uHits;
			m_iHitlistPos = (uint64_t(pDoc->m_uHits)<<32) + pDoc->m_uHit;
			m_bAllFieldsKnown = false;
			return m_tMatch;
		}
	}

	virtual void SeekHitlist ( SphOffset_t uOff )
	{
		int iHits = (int)(uOff>>32);
		if ( iHits==1 )
		{
			m_uNextHit = DWORD(uOff);
		} else
		{
			m_uNextHit = 0;
			m_tHitReader.Seek ( DWORD(uOff), iHits );
		}
	}

	virtual Hitpos_t GetNextHit ()
	{
		if ( m_uNextHit==0 )
		{
			return Hitpos_t ( m_tHitReader.UnzipHit() );

		} else if ( m_uNextHit==0xffffffffUL )
		{
			return EMPTY_HIT;

		} else
		{
			DWORD uRes = m_uNextHit;
			m_uNextHit = 0xffffffffUL;
			return Hitpos_t ( uRes );
		}
	}

	virtual bool Setup ( const RtIndex_t * pIndex, int iSegment, const SphChunkGuard_t & tGuard )
	{
		return pIndex->RtQwordSetup ( this, iSegment, tGuard );
	}

	void SetupReader ( const RtSegment_t * pSeg, const RtWord_t & tWord, const CSphFixedVector<SphDocID_t> & dKill )
	{
		m_tDocReader = RtDocReader_t ( pSeg, tWord );
		m_tHitReader.m_pBase = pSeg->m_dHits.Begin();
		m_pKill = m_pKillEnd = NULL;
		if ( dKill.GetLength()>0 )
		{
			m_pKill = dKill.Begin();
			m_pKillEnd = m_pKill + dKill.GetLength() - 1;
		}
	}

private:
	RtDocReader_t		m_tDocReader;
	CSphMatch			m_tMatch;

	DWORD				m_uNextHit;
	RtHitReader2_t		m_tHitReader;

	const SphDocID_t *	m_pKill;
	const SphDocID_t *	m_pKillEnd;
};


//////////////////////////////////////////////////////////////////////////


struct RtSubstringPayload_t : public ISphSubstringPayload
{
	RtSubstringPayload_t ( int iSegmentCount, int iDoclists )
		: m_dSegment2Doclists ( iSegmentCount )
		, m_dDoclist ( iDoclists )
	{}
	CSphFixedVector<Slice_t>	m_dSegment2Doclists;
	CSphFixedVector<Slice_t>	m_dDoclist;
};


struct RtQwordPayload_t : public RtQwordTraits_t
{
public:
	explicit RtQwordPayload_t ( const RtSubstringPayload_t * pPayload )
		: m_pPayload ( pPayload )
	{
		m_tMatch.Reset ( 0 );
		m_iDocs = m_pPayload->m_iTotalDocs;
		m_iHits = m_pPayload->m_iTotalHits;

		m_uDoclist = 0;
		m_uDoclistLeft = 0;
		m_pSegment = NULL;
		m_uHitEmbeded = EMPTY_HIT;
		m_pKill = m_pKillEnd = NULL;
	}

	virtual ~RtQwordPayload_t ()
	{}

	virtual const CSphMatch & GetNextDoc ( DWORD * )
	{
		m_iHits = 0;
		while (true)
		{
			const RtDoc_t * pDoc = m_tDocReader.UnzipDoc();
			if ( !pDoc && !m_uDoclistLeft )
			{
				m_tMatch.m_uDocID = 0;
				return m_tMatch;
			}

			if ( !pDoc && m_uDoclistLeft )
			{
				SetupReader();
				pDoc = m_tDocReader.UnzipDoc();
				assert ( pDoc );
			}

			if ( sphBinarySearch ( m_pKill, m_pKillEnd, pDoc->m_uDocID )!=NULL )
				continue;

			m_tMatch.m_uDocID = pDoc->m_uDocID;
			m_dQwordFields.Assign32 ( pDoc->m_uDocFields );
			m_bAllFieldsKnown = false;

			m_iHits = pDoc->m_uHits;
			m_uHitEmbeded = pDoc->m_uHit;
			m_tHitReader = RtHitReader_t ( m_pSegment, pDoc );

			return m_tMatch;
		}
	}

	virtual void SeekHitlist ( SphOffset_t )
	{}

	virtual Hitpos_t GetNextHit ()
	{
		if ( m_iHits>1 )
			return Hitpos_t ( m_tHitReader.UnzipHit() );
		else if ( m_iHits==1 )
		{
			Hitpos_t tHit ( m_uHitEmbeded );
			m_uHitEmbeded = EMPTY_HIT;
			return tHit;
		} else
		{
			return EMPTY_HIT;
		}
	}

	virtual bool Setup ( const RtIndex_t *, int iSegment, const SphChunkGuard_t & tGuard )
	{
		m_uDoclist = 0;
		m_uDoclistLeft = 0;
		m_tDocReader = RtDocReader_t();
		m_pSegment = NULL;
		m_pKill = m_pKillEnd = NULL;

		if ( iSegment<0 )
			return false;

		m_pSegment = tGuard.m_dRamChunks[iSegment];
		const KlistRefcounted_t * pKill = tGuard.m_dKill[iSegment];
		if ( pKill->m_dKilled.GetLength() )
		{
			m_pKill = pKill->m_dKilled.Begin();
			m_pKillEnd = m_pKill + pKill->m_dKilled.GetLength() - 1;
		}

		m_uDoclist = m_pPayload->m_dSegment2Doclists[iSegment].m_uOff;
		m_uDoclistLeft = m_pPayload->m_dSegment2Doclists[iSegment].m_uLen;

		if ( !m_uDoclistLeft )
			return false;

		SetupReader();
		return true;
	}

private:
	void SetupReader ()
	{
		assert ( m_uDoclistLeft );
		RtWord_t tWord;
		tWord.m_uDoc = m_pPayload->m_dDoclist[m_uDoclist].m_uOff;
		tWord.m_uDocs = m_pPayload->m_dDoclist[m_uDoclist].m_uLen;
		m_tDocReader = RtDocReader_t ( m_pSegment, tWord );
		m_uDoclist++;
		m_uDoclistLeft--;
	}

	const RtSubstringPayload_t *	m_pPayload;
	CSphMatch					m_tMatch;
	RtDocReader_t				m_tDocReader;
	RtHitReader_t				m_tHitReader;
	const RtSegment_t *			m_pSegment;
	const SphDocID_t *			m_pKill;
	const SphDocID_t *			m_pKillEnd;

	DWORD						m_uDoclist;
	DWORD						m_uDoclistLeft;
	DWORD						m_uHitEmbeded;
};


//////////////////////////////////////////////////////////////////////////

class RtQwordSetup_t : public ISphQwordSetup
{
public:
	explicit RtQwordSetup_t ( const SphChunkGuard_t & tGuard );
	virtual ISphQword *	QwordSpawn ( const XQKeyword_t & ) const final;
	virtual bool		QwordSetup ( ISphQword * pQword ) const final;
	void				SetSegment ( int iSegment ) { m_iSeg = iSegment; }

private:
	const SphChunkGuard_t & m_tGuard;
	int					m_iSeg;
};


RtQwordSetup_t::RtQwordSetup_t ( const SphChunkGuard_t & tGuard )
	: m_tGuard ( tGuard )
	, m_iSeg ( -1 )
{ }


ISphQword * RtQwordSetup_t::QwordSpawn ( const XQKeyword_t & tWord ) const
{
	if ( !tWord.m_pPayload )
		return new RtQword_t ();
	else
		return new RtQwordPayload_t ( (const RtSubstringPayload_t *)tWord.m_pPayload );
}


bool RtQwordSetup_t::QwordSetup ( ISphQword * pQword ) const
{
	// there was two dynamic_casts here once but they're not necessary
	// maybe it's worth to rewrite class hierarchy to avoid c-casts here?
	RtQwordTraits_t * pMyWord = (RtQwordTraits_t*)pQword;
	const RtIndex_t * pIndex = (const RtIndex_t *)m_pIndex;
	return pMyWord->Setup ( pIndex, m_iSeg, m_tGuard );
}


static void CopyDocinfo ( CSphMatch & tMatch, const DWORD * pFound )
{
	if ( !pFound )
		return;

	// setup static pointer
	assert ( DOCINFO2ID(pFound)==tMatch.m_uDocID );
	tMatch.m_pStatic = DOCINFO2ATTRS(pFound);

	// FIXME? implement overrides
}


const CSphRowitem * FindDocinfo ( const RtSegment_t * pSeg, SphDocID_t uDocID, int iStride )
{
	// FIXME! move to CSphIndex, and implement hashing
	if ( pSeg->m_dRows.GetLength()==0 )
		return NULL;

	int iStart = 0;
	int iEnd = pSeg->m_iRows-1;

	const CSphRowitem * pStorage = pSeg->m_dRows.Begin();
	const CSphRowitem * pFound = NULL;

	if ( uDocID==DOCINFO2ID ( &pStorage [ iStart*iStride ] ) )
	{
		pFound = &pStorage [ iStart*iStride ];

	} else if ( uDocID==DOCINFO2ID ( &pStorage [ iEnd*iStride ] ) )
	{
		pFound = &pStorage [ iEnd*iStride ];

	} else
	{
		while ( iEnd-iStart>1 )
		{
			// check if nothing found
			if (
				uDocID < DOCINFO2ID ( &pStorage [ iStart*iStride ] ) ||
				uDocID > DOCINFO2ID ( &pStorage [ iEnd*iStride ] ) )
				break;
			assert ( uDocID > DOCINFO2ID ( &pStorage [ iStart*iStride ] ) );
			assert ( uDocID < DOCINFO2ID ( &pStorage [ iEnd*iStride ] ) );

			int iMid = iStart + (iEnd-iStart)/2;
			if ( uDocID==DOCINFO2ID ( &pStorage [ iMid*iStride ] ) )
			{
				pFound = &pStorage [ iMid*iStride ];
				break;
			}
			if ( uDocID<DOCINFO2ID ( &pStorage [ iMid*iStride ] ) )
				iEnd = iMid;
			else
				iStart = iMid;
		}
	}

	return pFound;
}


bool RtIndex_t::EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const
{
	// might be needed even when we do not have a filter!
	if ( pCtx->m_bLookupFilter || pCtx->m_bLookupSort )
	{
		assert ( m_iStride==( DOCINFO_IDSIZE + m_tSchema.GetRowSize() ) );
		const CSphRowitem * pRow = FindDocinfo ( (RtSegment_t*)pCtx->m_pIndexData, tMatch.m_uDocID, m_iStride );
		if ( !pRow )
		{
			pCtx->m_iBadRows++;
			return true;
		}
		CopyDocinfo ( tMatch, pRow );
	}

	pCtx->CalcFilter ( tMatch );
	if ( !pCtx->m_pFilter )
		return false;

	if ( !pCtx->m_pFilter->Eval ( tMatch ) )
	{
		pCtx->FreeDataFilter ( tMatch );
		return true;
	}
	return false;
}


// WARNING, setup is pretty tricky
// for RT queries, we setup qwords several times
// first pass (with NULL segment arg) should sum all stats over all segments
// others passes (with non-NULL segments) should setup specific segment (including local stats)
bool RtIndex_t::RtQwordSetupSegment ( RtQword_t * pQword, const RtSegment_t * pCurSeg, bool bSetup, bool bWordDict, int iWordsCheckpoint, const CSphFixedVector<SphDocID_t> & dKill, const CSphIndexSettings & tSettings )
{
	if ( !pCurSeg )
		return false;

	SphWordID_t uWordID = pQword->m_uWordID;
	const char * sWord = pQword->m_sDictWord.cstr();
	int iWordLen = pQword->m_sDictWord.Length();
	bool bPrefix = false;
	if ( bWordDict && iWordLen && sWord[iWordLen-1]=='*' ) // crc star search emulation
	{
		iWordLen = iWordLen-1;
		bPrefix = true;
	}

	if ( !iWordLen )
		return false;

	// prevent prefix matching for explicitly setting prohibited by config, to be on pair with plain index (or CRC kind of index)
	if ( bPrefix && ( ( tSettings.m_iMinPrefixLen && iWordLen<tSettings.m_iMinPrefixLen ) || ( tSettings.m_iMinInfixLen && iWordLen<tSettings.m_iMinInfixLen ) ) )
		return false;

	// no checkpoints - check all words
	// no checkpoints matched - check only words prior to 1st checkpoint
	// checkpoint found - check words at that checkpoint
	RtWordReader_t tReader ( pCurSeg, bWordDict, iWordsCheckpoint );

	if ( pCurSeg->m_dWordCheckpoints.GetLength() )
	{
		const RtWordCheckpoint_t * pCp = sphSearchCheckpoint ( sWord, iWordLen, uWordID, false, bWordDict
			, pCurSeg->m_dWordCheckpoints.Begin(), &pCurSeg->m_dWordCheckpoints.Last() );

		const BYTE * pWords = pCurSeg->m_dWords.Begin();

		if ( !pCp )
		{
			tReader.m_pMax = pWords + pCurSeg->m_dWordCheckpoints.Begin()->m_iOffset;
		} else
		{
			tReader.m_pCur = pWords + pCp->m_iOffset;
			// if next checkpoint exists setup reader range
			if ( ( pCp+1 )<= ( &pCurSeg->m_dWordCheckpoints.Last() ) )
				tReader.m_pMax = pWords + pCp[1].m_iOffset;
		}
	}

	// find the word between checkpoints
	const RtWord_t * pWord = NULL;
	while ( ( pWord = tReader.UnzipWord() )!=NULL )
	{
		int iCmp = 0;
		if ( bWordDict )
		{
			iCmp = sphDictCmpStrictly ( (const char *)pWord->m_sWord+1, pWord->m_sWord[0], sWord, iWordLen );
		} else
		{
			if ( pWord->m_uWordID<uWordID )
				iCmp = -1;
			else if ( pWord->m_uWordID>uWordID )
				iCmp = 1;
		}

		if ( iCmp==0 )
		{
			pQword->m_iDocs += pWord->m_uDocs;
			pQword->m_iHits += pWord->m_uHits;
			if ( bSetup )
				pQword->SetupReader ( pCurSeg, *pWord, dKill );

			return true;

		} else if ( iCmp>0 )
			return false;
	}
	return false;
}

struct RtExpandedEntry_t
{
	DWORD	m_uHash;
	int		m_iNameOff;
	int		m_iDocs;
	int		m_iHits;
};

struct RtExpandedPayload_t
{
	int		m_iDocs;
	int		m_iHits;
	DWORD	m_uDoclistOff;
};

struct RtExpandedTraits_fn
{
	inline bool IsLess ( const RtExpandedEntry_t & a, const RtExpandedEntry_t & b ) const
	{
		assert ( m_sBase );
		if ( a.m_uHash!=b.m_uHash )
		{
			return a.m_uHash<b.m_uHash;
		} else
		{
			const BYTE * pA = m_sBase + a.m_iNameOff;
			const BYTE * pB = m_sBase + b.m_iNameOff;
			if ( pA[0]!=pB[0] )
				return pA[0]<pB[0];

			return ( sphDictCmp ( (const char *)pA+1, pA[0], (const char *)pB+1, pB[0] )<0 );
		}
	}

	inline bool IsEqual ( const RtExpandedEntry_t * a, const RtExpandedEntry_t * b ) const
	{
		assert ( m_sBase );
		if ( a->m_uHash!=b->m_uHash )
			return false;

		const BYTE * pA = m_sBase + a->m_iNameOff;
		const BYTE * pB = m_sBase + b->m_iNameOff;
		if ( pA[0]!=pB[0] )
			return false;

		return ( sphDictCmp ( (const char *)pA+1, pA[0], (const char *)pB+1, pB[0] )==0 );
	}

	explicit RtExpandedTraits_fn ( const BYTE * sBase )
		: m_sBase ( sBase )
	{ }
	const BYTE * m_sBase;
};


struct DictEntryRtPayload_t
{
	DictEntryRtPayload_t ( bool bPayload, int iSegments )
	{
		m_bPayload = bPayload;
		m_iSegExpansionLimit = iSegments;
		if ( bPayload )
		{
			m_dWordPayload.Reserve ( 1000 );
			m_dSeg.Resize ( iSegments );
			ARRAY_FOREACH ( i, m_dSeg )
			{
				m_dSeg[i].m_uOff = 0;
				m_dSeg[i].m_uLen = 0;
			}
		}

		m_dWordExpand.Reserve ( 1000 );
		m_dWordBuf.Reserve ( 8096 );
	}

	void Add ( const RtWord_t * pWord, int iSegment )
	{
		if ( !m_bPayload || !sphIsExpandedPayload ( pWord->m_uDocs, pWord->m_uHits ) )
		{
			RtExpandedEntry_t & tExpand = m_dWordExpand.Add();

			int iOff = m_dWordBuf.GetLength();
			int iWordLen = pWord->m_sWord[0] + 1;
			tExpand.m_uHash = sphCRC32 ( pWord->m_sWord, iWordLen );
			tExpand.m_iNameOff = iOff;
			tExpand.m_iDocs = pWord->m_uDocs;
			tExpand.m_iHits = pWord->m_uHits;
			m_dWordBuf.Append ( pWord->m_sWord, iWordLen );
		} else
		{
			RtExpandedPayload_t & tExpand = m_dWordPayload.Add();
			tExpand.m_iDocs = pWord->m_uDocs;
			tExpand.m_iHits = pWord->m_uHits;
			tExpand.m_uDoclistOff = pWord->m_uDoc;

			m_dSeg[iSegment].m_uOff = m_dWordPayload.GetLength();
			m_dSeg[iSegment].m_uLen++;
		}
	}

	void Convert ( ISphWordlist::Args_t & tArgs )
	{
		if ( !m_dWordExpand.GetLength() && !m_dWordPayload.GetLength() )
			return;

		int iTotalDocs = 0;
		int iTotalHits = 0;
		if ( m_dWordExpand.GetLength() )
		{
			int iRtExpansionLimit = tArgs.m_iExpansionLimit * m_iSegExpansionLimit;
			if ( tArgs.m_iExpansionLimit && m_dWordExpand.GetLength()>iRtExpansionLimit )
			{
				// sort expansions by frequency desc
				// clip the less frequent ones if needed, as they are likely misspellings
				sphSort ( m_dWordExpand.Begin(), m_dWordExpand.GetLength(), ExpandedOrderDesc_T<RtExpandedEntry_t>() );
				m_dWordExpand.Resize ( iRtExpansionLimit );
			}

			// lets merge statistics for same words from different segments as hash produce a lot tiny allocations here
			const BYTE * sBase = m_dWordBuf.Begin();
			RtExpandedTraits_fn fnCmp ( sBase );
			sphSort ( m_dWordExpand.Begin(), m_dWordExpand.GetLength(), fnCmp );

			const RtExpandedEntry_t * pLast = m_dWordExpand.Begin();
			tArgs.AddExpanded ( sBase+pLast->m_iNameOff+1, sBase[pLast->m_iNameOff], pLast->m_iDocs, pLast->m_iHits );
			for ( int i=1; i<m_dWordExpand.GetLength(); i++ )
			{
				const RtExpandedEntry_t * pCur = m_dWordExpand.Begin() + i;

				if ( fnCmp.IsEqual ( pLast, pCur ) )
				{
					tArgs.m_dExpanded.Last().m_iDocs += pCur->m_iDocs;
					tArgs.m_dExpanded.Last().m_iHits += pCur->m_iHits;
				} else
				{
					tArgs.AddExpanded ( sBase + pCur->m_iNameOff + 1, sBase[pCur->m_iNameOff], pCur->m_iDocs, pCur->m_iHits );
					pLast = pCur;
				}
				iTotalDocs += pCur->m_iDocs;
				iTotalHits += pCur->m_iHits;
			}
		}

		if ( m_dWordPayload.GetLength() )
		{
			DWORD uExpansionLimit = tArgs.m_iExpansionLimit;
			int iPayloads = 0;
			ARRAY_FOREACH ( i, m_dSeg )
			{
				Slice_t & tSeg = m_dSeg[i];

				// reverse per segment offset to payload doc-list as offset was the end instead of start
				assert ( tSeg.m_uOff>=tSeg.m_uLen );
				tSeg.m_uOff = tSeg.m_uOff - tSeg.m_uLen;

				// per segment expansion limit clip
				if ( uExpansionLimit && tSeg.m_uLen>uExpansionLimit )
				{
					// sort expansions by frequency desc
					// per segment clip the less frequent ones if needed, as they are likely misspellings
					sphSort ( m_dWordPayload.Begin()+tSeg.m_uOff, tSeg.m_uLen, ExpandedOrderDesc_T<RtExpandedPayload_t>() );
					tSeg.m_uLen = uExpansionLimit;
				}

				iPayloads += tSeg.m_uLen;
				// sort by ascending doc-list offset
				sphSort ( m_dWordPayload.Begin()+tSeg.m_uOff, tSeg.m_uLen, bind ( &RtExpandedPayload_t::m_uDoclistOff ) );
			}

			auto * pPayload = new RtSubstringPayload_t ( m_dSeg.GetLength(), iPayloads );

			Slice_t * pDst = pPayload->m_dDoclist.Begin();
			ARRAY_FOREACH ( i, m_dSeg )
			{
				const Slice_t & tSeg = m_dSeg[i];
				const RtExpandedPayload_t * pSrc = m_dWordPayload.Begin() + tSeg.m_uOff;
				const RtExpandedPayload_t * pEnd = pSrc + tSeg.m_uLen;
				pPayload->m_dSegment2Doclists[i].m_uOff = pDst - pPayload->m_dDoclist.Begin();
				pPayload->m_dSegment2Doclists[i].m_uLen = tSeg.m_uLen;
				while ( pSrc!=pEnd )
				{
					pDst->m_uOff = pSrc->m_uDoclistOff;
					pDst->m_uLen = pSrc->m_iDocs;
					iTotalDocs += pSrc->m_iDocs;
					iTotalHits += pSrc->m_iHits;
					pDst++;
					pSrc++;
				}
			}
			pPayload->m_iTotalDocs = iTotalDocs;
			pPayload->m_iTotalHits = iTotalHits;
			tArgs.m_pPayload = pPayload;
		}

		tArgs.m_iTotalDocs = iTotalDocs;
		tArgs.m_iTotalHits = iTotalHits;
	}

	bool							m_bPayload;
	CSphVector<RtExpandedEntry_t>	m_dWordExpand;
	CSphVector<RtExpandedPayload_t>	m_dWordPayload;
	CSphVector<BYTE>				m_dWordBuf;
	CSphVector<Slice_t>				m_dSeg;
	int								m_iSegExpansionLimit = 0;
};


void RtIndex_t::GetPrefixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const
{
	int dWildcard [ SPH_MAX_WORD_LEN + 1 ];
	int * pWildcard = ( sphIsUTF8 ( sWildcard ) && sphUTF8ToWideChar ( sWildcard, dWildcard, SPH_MAX_WORD_LEN ) ) ? dWildcard : NULL;

	const CSphFixedVector<RtSegment_t*> & dSegments = *((CSphFixedVector<RtSegment_t*> *)tArgs.m_pIndexData);
	DictEntryRtPayload_t tDict2Payload ( tArgs.m_bPayload, dSegments.GetLength() );
	const int iSkipMagic = ( BYTE(*sSubstring)<0x20 ); // whether to skip heading magic chars in the prefix, like NONSTEMMED maker
	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pCurSeg = dSegments[iSeg];
		RtWordReader_t tReader ( pCurSeg, true, m_iWordsCheckpoint );

		// find initial checkpoint or check words prior to 1st checkpoint
		if ( pCurSeg->m_dWordCheckpoints.GetLength() )
		{
			const RtWordCheckpoint_t * pCurCheckpoint = sphSearchCheckpoint ( sSubstring, iSubLen, 0, true, true
				, pCurSeg->m_dWordCheckpoints.Begin(), &pCurSeg->m_dWordCheckpoints.Last() );

			if ( pCurCheckpoint )
			{
				// there could be valid data prior 1st checkpoint that should be unpacked and checked
				int iCheckpointNameLen = strlen ( pCurCheckpoint->m_sWord );
				if ( pCurCheckpoint!=pCurSeg->m_dWordCheckpoints.Begin()
					|| ( sphDictCmp ( sSubstring, iSubLen, pCurCheckpoint->m_sWord, iCheckpointNameLen )==0 && iSubLen==iCheckpointNameLen ) )
				{
					tReader.m_pCur = pCurSeg->m_dWords.Begin() + pCurCheckpoint->m_iOffset;
				}
			}
		}

		// find the word between checkpoints
		const RtWord_t * pWord = NULL;
		while ( ( pWord = tReader.UnzipWord() )!=NULL )
		{
			int iCmp = sphDictCmp ( sSubstring, iSubLen, (const char *)pWord->m_sWord+1, pWord->m_sWord[0] );
			if ( iCmp<0 )
			{
				break;
			} else if ( iCmp==0 && iSubLen<=pWord->m_sWord[0] && sphWildcardMatch ( (const char *)pWord->m_sWord+1+iSkipMagic, sWildcard, pWildcard ) )
			{
				tDict2Payload.Add ( pWord, iSeg );
			}
			// FIXME!!! same case 'boxi*' matches 'box' document at plain index
			// but masked by a checkpoint search
		}
	}

	tDict2Payload.Convert ( tArgs );
}


static bool ExtractInfixCheckpoints ( const char * sInfix, int iBytes, int iMaxCodepointLength, int iDictCpCount, const CSphTightVector<uint64_t> & dFilter, CSphVector<DWORD> & dCheckpoints )
{
	if ( !dFilter.GetLength() )
		return false;

	int iStart = dCheckpoints.GetLength();

	uint64_t dVals[ BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT ];
	memset ( dVals, 0, sizeof(dVals) );

	BloomGenTraits_t tBloom0 ( dVals );
	BloomGenTraits_t tBloom1 ( dVals+BLOOM_PER_ENTRY_VALS_COUNT );
	if ( !BuildBloom ( (const BYTE *)sInfix, iBytes, BLOOM_NGRAM_0, ( iMaxCodepointLength>1 ), BLOOM_PER_ENTRY_VALS_COUNT, tBloom0 ) )
		return false;
	BuildBloom ( (const BYTE *)sInfix, iBytes, BLOOM_NGRAM_1, ( iMaxCodepointLength>1 ), BLOOM_PER_ENTRY_VALS_COUNT, tBloom1 );

	for ( int iDictCp=0; iDictCp<iDictCpCount+1; iDictCp++ )
	{
		const uint64_t * pCP = dFilter.Begin() + iDictCp * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
		const uint64_t * pSuffix = dVals;

		bool bMatched = true;
		for ( int iElem=0; iElem<BLOOM_PER_ENTRY_VALS_COUNT*BLOOM_HASHES_COUNT; iElem++ )
		{
			uint64_t uFilter = *pCP++;
			uint64_t uSuffix = *pSuffix++;
			if ( ( uFilter & uSuffix )!=uSuffix )
			{
				bMatched = false;
				break;
			}
		}

		if ( bMatched )
			dCheckpoints.Add ( (DWORD)iDictCp );
	}

	return ( dCheckpoints.GetLength()!=iStart );
}


void RtIndex_t::GetInfixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const
{
	// sanity checks
	if ( !sSubstring || iSubLen<=0 )
		return;

	// find those prefixes
	CSphVector<DWORD> dPoints;
	const int iSkipMagic = ( tArgs.m_bHasMorphology ? 1 : 0 ); // whether to skip heading magic chars in the prefix, like NONSTEMMED maker
	const CSphFixedVector<RtSegment_t*> & dSegments = *((CSphFixedVector<RtSegment_t*> *)tArgs.m_pIndexData);

	DictEntryRtPayload_t tDict2Payload ( tArgs.m_bPayload, dSegments.GetLength() );
	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pSeg = dSegments[iSeg];
		if ( !pSeg->m_dWords.GetLength() )
			continue;

		dPoints.Resize ( 0 );
		if ( !ExtractInfixCheckpoints ( sSubstring, iSubLen, m_iMaxCodepointLength, pSeg->m_dWordCheckpoints.GetLength(), pSeg->m_dInfixFilterCP, dPoints ) )
			continue;

		int dWildcard [ SPH_MAX_WORD_LEN + 1 ];
		int * pWildcard = ( sphIsUTF8 ( sWildcard ) && sphUTF8ToWideChar ( sWildcard, dWildcard, SPH_MAX_WORD_LEN ) ) ? dWildcard : NULL;

		// walk those checkpoints, check all their words
		ARRAY_FOREACH ( i, dPoints )
		{
			int iNext = (int)dPoints[i];
			int iCur = iNext-1;
			RtWordReader_t tReader ( pSeg, true, m_iWordsCheckpoint );
			if ( iCur>0 )
				tReader.m_pCur = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iCur].m_iOffset;
			if ( iNext<pSeg->m_dWordCheckpoints.GetLength() )
				tReader.m_pMax = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iNext].m_iOffset;

			const RtWord_t * pWord = NULL;
			while ( ( pWord = tReader.UnzipWord() )!=NULL )
			{
				if ( tArgs.m_bHasMorphology && pWord->m_sWord[1]!=MAGIC_WORD_HEAD_NONSTEMMED )
					continue;

				// check it
				if ( !sphWildcardMatch ( (const char*)pWord->m_sWord+1+iSkipMagic, sWildcard, pWildcard ) )
					continue;

				// matched, lets add
				tDict2Payload.Add ( pWord, iSeg );
			}
		}
	}

	tDict2Payload.Convert ( tArgs );
}

void RtIndex_t::GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const
{
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	const CSphFixedVector<const RtSegment_t*> & dSegments = tGuard.m_dRamChunks;

	// segments and disk chunks dictionaries produce duplicated entries
	tRes.m_bMergeWords = true;

	if ( dSegments.GetLength() )
	{
		assert ( !tRes.m_pWordReader && !tRes.m_pSegments );
		tRes.m_pWordReader = new RtWordReader_t ( dSegments[0], true, m_iWordsCheckpoint );
		tRes.m_pSegments = &tGuard.m_dRamChunks;
		tRes.m_bHasExactDict = m_tSettings.m_bIndexExactWords;

		// FIXME!!! cache InfixCodepointBytes as it is slow - GetMaxCodepointLength is charset_table traverse
		sphGetSuggest ( this, m_pTokenizer->GetMaxCodepointLength(), tArgs, tRes );

		auto pReader = ( RtWordReader_t * ) tRes.m_pWordReader;
		SafeDelete ( pReader );
		tRes.m_pWordReader = NULL;
		tRes.m_pSegments = NULL;
	}

	int iWorstCount = 0;
	// check disk chunks from recent to oldest
	for ( int i=tGuard.m_dDiskChunks.GetLength()-1; i>=0; i-- )
	{
		int iWorstDist = 0;
		int iWorstDocs = 0;
		if ( tRes.m_dMatched.GetLength() )
		{
			iWorstDist = tRes.m_dMatched.Last().m_iDistance;
			iWorstDocs = tRes.m_dMatched.Last().m_iDocs;
		}

		tGuard.m_dDiskChunks[i]->GetSuggest ( tArgs, tRes );

		// stop checking in case worst element is same several times during loop
		if ( tRes.m_dMatched.GetLength() && iWorstDist==tRes.m_dMatched.Last().m_iDistance && iWorstDocs==tRes.m_dMatched.Last().m_iDocs )
		{
			iWorstCount++;
			if ( iWorstCount>2 )
				break;
		} else
		{
			iWorstCount = 0;
		}
	}
}

void RtIndex_t::SuffixGetChekpoints ( const SuggestResult_t & tRes, const char * sSuffix, int iLen, CSphVector<DWORD> & dCheckpoints ) const
{
	const CSphFixedVector<const RtSegment_t*> & dSegments = *( (const CSphFixedVector<const RtSegment_t*> *)tRes.m_pSegments );
	assert ( dSegments.GetLength()<0xFF );

	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pSeg = dSegments[iSeg];
		if ( !pSeg->m_dWords.GetLength () )
			continue;

		int iStart = dCheckpoints.GetLength();
		if ( !ExtractInfixCheckpoints ( sSuffix, iLen, m_iMaxCodepointLength, pSeg->m_dWordCheckpoints.GetLength(), pSeg->m_dInfixFilterCP, dCheckpoints ) )
			continue;

		DWORD iSegPacked = (DWORD)iSeg<<24;
		for ( int i=iStart; i<dCheckpoints.GetLength(); i++ )
		{
			assert ( ( dCheckpoints[i] & 0xFFFFFF )==dCheckpoints[i] );
			dCheckpoints[i] |= iSegPacked;
		}
	}
}

void RtIndex_t::SetCheckpoint ( SuggestResult_t & tRes, DWORD iCP ) const
{
	assert ( tRes.m_pWordReader && tRes.m_pSegments );
	const CSphFixedVector<const RtSegment_t*> & dSegments = *( (const CSphFixedVector<const RtSegment_t*> *)tRes.m_pSegments );
	RtWordReader_t * pReader = (RtWordReader_t *)tRes.m_pWordReader;

	int iSeg = iCP>>24;
	assert ( iSeg>=0 && iSeg<dSegments.GetLength() );
	const RtSegment_t * pSeg = dSegments[iSeg];
	pReader->Reset ( pSeg );

	int iNext = (int)( iCP & 0xFFFFFF );
	int iCur = iNext-1;

	if ( iCur>0 )
		pReader->m_pCur = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iCur].m_iOffset;
	if ( iNext<pSeg->m_dWordCheckpoints.GetLength() )
		pReader->m_pMax = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iNext].m_iOffset;
}

bool RtIndex_t::ReadNextWord ( SuggestResult_t & tRes, DictWord_t & tWord ) const
{
	assert ( tRes.m_pWordReader );
	RtWordReader_t * pReader = (RtWordReader_t *)tRes.m_pWordReader;

	const RtWord_t * pWord = pReader->UnzipWord();

	if ( !pWord )
		return false;

	tWord.m_sWord = (const char *)( pWord->m_sWord + 1 );
	tWord.m_iLen = pWord->m_sWord[0];
	tWord.m_iDocs = pWord->m_uDocs;
	return true;
}


bool RtIndex_t::RtQwordSetup ( RtQword_t * pQword, int iSeg, const SphChunkGuard_t & tGuard ) const
{
	// segment-specific setup pass
	if ( iSeg>=0 )
		return RtQwordSetupSegment ( pQword, tGuard.m_dRamChunks[iSeg], true, m_bKeywordDict, m_iWordsCheckpoint, tGuard.m_dKill[iSeg]->m_dKilled, m_tSettings );

	// stat-only pass
	// loop all segments, gather stats, do not setup anything
	pQword->m_iDocs = 0;
	pQword->m_iHits = 0;
	if ( !tGuard.m_dRamChunks.GetLength() )
		return true;

	// we care about the results anyway though
	// because if all (!) segments miss this word, we must notify the caller, right?
	bool bFound = false;
	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
		bFound |= RtQwordSetupSegment ( pQword, tGuard.m_dRamChunks[i], false, m_bKeywordDict, m_iWordsCheckpoint, tGuard.m_dKill[i]->m_dKilled, m_tSettings );

	// sanity check
	assert (!( bFound==true && pQword->m_iDocs==0 ) );
	return bFound;
}


bool RtIndex_t::IsStarDict() const
{
	return m_tSettings.m_iMinPrefixLen>0 || m_tSettings.m_iMinInfixLen>0;
}


static void SetupExactDict ( CSphDictRefPtr_c &pDict, ISphTokenizer * pTokenizer, bool bAddSpecial=true )
{
	assert ( pTokenizer );
	pTokenizer->AddPlainChar ( '=' );
	if ( bAddSpecial )
		pTokenizer->AddSpecials ( "=" );
	pDict = new CSphDictExact ( pDict );
}


static void SetupStarDict ( CSphDictRefPtr_c& pDict, ISphTokenizer * pTokenizer )
{
	assert ( pTokenizer );
	pTokenizer->AddPlainChar ( '*' );
	pDict = new CSphDictStarV8 ( pDict, true );
}

struct SphRtFinalMatchCalc_t : ISphMatchProcessor, ISphNoncopyable // fixme! that is actually class, not struct.
{
private:
	const CSphQueryContext &	m_tCtx;
	int							m_iSeg;
	int							m_iSegments;
	// count per segments matches
	// to skip iteration of matches at sorter and pool setup for segment without matches at sorter
	CSphBitvec					m_dSegments;

public:
	SphRtFinalMatchCalc_t ( int iSegments, const CSphQueryContext & tCtx )
		: m_tCtx ( tCtx )
		, m_iSeg ( 0 )
		, m_iSegments ( iSegments )
	{
		m_dSegments.Init ( iSegments );
	}

	bool NextSegment ( int iSeg )
	{
		m_iSeg = iSeg;

		bool bSegmentGotRows = m_dSegments.BitGet ( iSeg );

		// clear current row
		m_dSegments.BitClear ( iSeg );

		// also clear 0 segment as it got forced to process
		m_dSegments.BitClear ( 0 );

		// also force to process 0 segment to mark all used segments
		return ( iSeg==0 || bSegmentGotRows );
	}

	bool HasSegments () const
	{
		return ( m_iSeg==0 || m_dSegments.BitCount()>0 );
	}

	void Process ( CSphMatch * pMatch ) final
	{
		int iMatchSegment = pMatch->m_iTag-1;
		if ( iMatchSegment==m_iSeg && pMatch->m_pStatic )
			m_tCtx.CalcFinal ( *pMatch );

		// count all used segments at 0 pass
		if ( m_iSeg==0 && iMatchSegment<m_iSegments )
			m_dSegments.BitSet ( iMatchSegment );
	}
};


class RTMatchesToNewSchema_c : public MatchesToNewSchema_c
{
public:
	RTMatchesToNewSchema_c ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema, const SphChunkGuard_t & tGuard, const CSphVector<const DWORD *> & dMVA, const CSphVector<const BYTE *> & dStrings, const CSphBitvec & tMvaArenaFlag )
		: MatchesToNewSchema_c ( pOldSchema, pNewSchema )
		, m_tGuard ( tGuard )
		, m_dDiskMVA ( dMVA )
		, m_dDiskStrings ( dStrings )
		, m_tMvaArenaFlag ( tMvaArenaFlag )
	{}

private:
	const SphChunkGuard_t &				m_tGuard;
	const CSphVector<const DWORD *> &	m_dDiskMVA;
	const CSphVector<const BYTE *> &	m_dDiskStrings;
	const CSphBitvec &					m_tMvaArenaFlag;

	const DWORD * GetMVAPool ( const CSphMatch * pMatch ) final
	{
		int nRamChunks = m_tGuard.m_dRamChunks.GetLength();
		int iChunkId = pMatch->m_iTag-1;
		if ( iChunkId < nRamChunks )
			return m_tGuard.m_dRamChunks[iChunkId]->m_dMvas.Begin();

		return m_dDiskMVA[iChunkId-nRamChunks];
	}

	const BYTE * GetStringPool ( const CSphMatch * pMatch ) final
	{
		int nRamChunks = m_tGuard.m_dRamChunks.GetLength();
		int iChunkId = pMatch->m_iTag-1;
		if ( iChunkId < nRamChunks )
			return m_tGuard.m_dRamChunks[iChunkId]->m_dStrings.Begin();

		return m_dDiskStrings[iChunkId-nRamChunks];
	}

	bool GetArenaProhibitFlag ( const CSphMatch * pMatch ) final
	{
		int nRamChunks = m_tGuard.m_dRamChunks.GetLength();
		int iChunkId = pMatch->m_iTag-1;
		if ( iChunkId < nRamChunks )
			return false;

		return m_tMvaArenaFlag.BitGet ( iChunkId-nRamChunks );
	}
};


static void TransformSorterSchema ( ISphMatchSorter * pSorter, const SphChunkGuard_t & tGuard, const CSphVector<const DWORD *> & dMVA, const CSphVector<const BYTE *> & dStrings, const CSphBitvec & tMvaArenaFlag )
{
	assert ( pSorter );

	const ISphSchema * pOldSchema = pSorter->GetSchema();
	ISphSchema * pNewSchema =  sphCreateStandaloneSchema ( pOldSchema );
	assert ( pOldSchema && pNewSchema );

	RTMatchesToNewSchema_c fnFinal ( pOldSchema, pNewSchema, tGuard, dMVA, dStrings, tMvaArenaFlag );
	pSorter->Finalize ( fnFinal, false );

	pSorter->SetSchema ( pNewSchema );
	SafeDelete ( pOldSchema );
}


void RtIndex_t::GetReaderChunks ( SphChunkGuard_t & tGuard ) const NO_THREAD_SAFETY_ANALYSIS
{
	if ( !m_dRamChunks.GetLength() && !m_dDiskChunks.GetLength() )
		return;

	m_tReading.ReadLock();
	tGuard.m_pReading = &m_tReading;

	m_tChunkLock.ReadLock ();

	tGuard.m_dRamChunks.Reset ( m_dRamChunks.GetLength () );
	tGuard.m_dKill.Reset ( m_dRamChunks.GetLength () );
	tGuard.m_dDiskChunks.Reset ( m_dDiskChunks.GetLength () );

	memcpy ( tGuard.m_dRamChunks.Begin (), m_dRamChunks.Begin (), m_dRamChunks.GetLengthBytes () );
	memcpy ( tGuard.m_dDiskChunks.Begin (), m_dDiskChunks.Begin (),	m_dDiskChunks.GetLengthBytes () );

	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
	{
		KlistRefcounted_t * pKlist = tGuard.m_dRamChunks[i]->m_pKlist;
		pKlist->AddRef();
		tGuard.m_dKill[i] = pKlist;

		assert ( tGuard.m_dRamChunks[i]->m_tRefCount.GetValue()>=0 );
		tGuard.m_dRamChunks[i]->m_tRefCount.Inc();
	}

	m_tChunkLock.Unlock ();
}


SphChunkGuard_t::~SphChunkGuard_t()
{
	if ( m_pReading )
		m_pReading->Unlock();

	if ( !m_dRamChunks.GetLength() )
		return;

	ARRAY_FOREACH ( i, m_dRamChunks )
	{
		assert ( m_dRamChunks[i]->m_tRefCount.GetValue()>=1 );

		KlistRefcounted_t * pKlist = const_cast<KlistRefcounted_t *> ( m_dKill[i] );
		SafeRelease ( pKlist );

		m_dRamChunks[i]->m_tRefCount.Dec();
	}
}


// FIXME! missing MVA, index_exact_words support
// FIXME? any chance to factor out common backend agnostic code?
// FIXME? do we need to support pExtraFilters?
bool RtIndex_t::MultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters,
	ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	assert ( ppSorters );
	assert ( pResult );

	// to avoid the checking of a ppSorters's element for NULL on every next step, just filter out all nulls right here
	CSphVector<ISphMatchSorter*> dSorters;
	dSorters.Reserve ( iSorters );
	for ( int i=0; i<iSorters; ++i )
		if ( ppSorters[i] )
			dSorters.Add ( ppSorters[i] );

	// if we have anything to work with
	if ( dSorters.GetLength()==0 )
	{
		pResult->m_iQueryTime = 0;
		return false;
	}

	assert ( pQuery );
	assert ( tArgs.m_iTag==0 );

	MEMORY ( MEM_RT_QUERY );

	// start counting
	pResult->m_iQueryTime = 0;
	int64_t tmQueryStart = sphMicroTimer();
	CSphQueryProfile * pProfiler = pResult->m_pProfile;
	ESphQueryState eOldState = SPH_QSTATE_UNKNOWN;

	if ( pProfiler )
		eOldState = pProfiler->Switch ( SPH_QSTATE_DICT_SETUP );

	// force ext2 mode for them
	// FIXME! eliminate this const breakage
	const_cast<CSphQuery*> ( pQuery )->m_eMode = SPH_MATCH_EXTENDED2;

	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	// wrappers
	ISphTokenizerRefPtr_c pQueryTokenizer { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
	sphSetupQueryTokenizer ( pQueryTokenizer, IsStarDict(), m_tSettings.m_bIndexExactWords, false );

	CSphDictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( m_bKeywordDict && IsStarDict () )
		SetupStarDict ( pDict, pQueryTokenizer );

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pQueryTokenizer );

	// calculate local idf for RT with disk chunks
	// in case of local_idf set but no external hash no full-scan query and RT has disk chunks
	const SmallStringHash_T<int64_t> * pLocalDocs = tArgs.m_pLocalDocs;
	SmallStringHash_T<int64_t> hLocalDocs;
	int64_t iTotalDocs = ( tArgs.m_iTotalDocs ? tArgs.m_iTotalDocs : m_tStats.m_iTotalDocuments );
	bool bGotLocalDF = tArgs.m_bLocalDF;
	if ( tArgs.m_bLocalDF && !tArgs.m_pLocalDocs && !pQuery->m_sQuery.IsEmpty() && tGuard.m_dDiskChunks.GetLength() )
	{
		if ( pProfiler )
			pProfiler->Switch ( SPH_QSTATE_LOCAL_DF );

		GetKeywordsSettings_t tSettings;
		tSettings.m_bStats = true;
		CSphVector < CSphKeywordInfo > dKeywords;
		DoGetKeywords ( dKeywords, pQuery->m_sQuery.cstr(), tSettings, false, NULL, tGuard );
		for ( auto & tKw : dKeywords )
			if ( !hLocalDocs.Exists ( tKw.m_sNormalized ) ) // skip dupes
				hLocalDocs.Add ( tKw.m_iDocs, tKw.m_sNormalized );

		pLocalDocs = &hLocalDocs;
		iTotalDocs = GetStats().m_iTotalDocuments;
		bGotLocalDF = true;
	}

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_INIT );

	// FIXME! each result will point to its own MVA and string pools

	//////////////////////
	// search disk chunks
	//////////////////////

	pResult->m_bHasPrediction = pQuery->m_iMaxPredictedMsec>0;

	SphWordStatChecker_t tDiskStat;
	SphWordStatChecker_t tStat;
	tStat.Set ( pResult->m_hWordStats );

	int64_t tmMaxTimer = 0;
	if ( pQuery->m_uMaxQueryMsec>0 )
		tmMaxTimer = sphMicroTimer() + pQuery->m_uMaxQueryMsec*1000; // max_query_time

	CSphVector<SphDocID_t> dCumulativeKList;
	KillListVector dMergedKillist;
	CSphVector<const BYTE *> dDiskStrings ( tGuard.m_dDiskChunks.GetLength() );
	CSphVector<const DWORD *> dDiskMva ( tGuard.m_dDiskChunks.GetLength() );
	CSphBitvec tMvaArenaFlag ( tGuard.m_dDiskChunks.GetLength() );
	if ( tGuard.m_dDiskChunks.GetLength() )
		m_tKlist.Flush ( dCumulativeKList );

	for ( int iChunk = tGuard.m_dDiskChunks.GetLength()-1; iChunk>=0; iChunk-- )
	{
		// because disk chunk search within the loop will switch the profiler state
		if ( pProfiler )
			pProfiler->Switch ( SPH_QSTATE_INIT );

		// collect & sort cumulative killlist for current chunk
		if ( iChunk<tGuard.m_dDiskChunks.GetLength()-1 )
		{
			const CSphIndex * pNewerChunk = tGuard.m_dDiskChunks [ iChunk+1 ];
			int iKlistEntries = pNewerChunk->GetKillListSize();
			if ( iKlistEntries )
			{
				// merging two kill lists, assuming they have sorted data
				const SphDocID_t * pSrc1 = dCumulativeKList.Begin();
				const SphDocID_t * pSrc2 = pNewerChunk->GetKillList();
				const SphDocID_t * pEnd1 = pSrc1 + dCumulativeKList.GetLength();
				const SphDocID_t * pEnd2 = pSrc2 + iKlistEntries;
				CSphVector<SphDocID_t> dNewCumulative ( ( pEnd1-pSrc1 )+( pEnd2-pSrc2 ) );
				SphDocID_t * pDst = dNewCumulative.Begin();

				while ( pSrc1!=pEnd1 && pSrc2!=pEnd2 )
				{
					if ( *pSrc1<*pSrc2 )
						*pDst = *pSrc1++;
					else if ( *pSrc2<*pSrc1 )
						*pDst = *pSrc2++;
					else
					{
						*pDst = *pSrc1++;
						// handle duplicates
						while ( pSrc1!=pEnd1 && *pDst==*pSrc1 ) pSrc1++;
						while ( pSrc2!=pEnd2 && *pDst==*pSrc2 ) pSrc2++;
					}
					pDst++;
				}
				while ( pSrc1!=pEnd1 ) *pDst++ = *pSrc1++;
				while ( pSrc2!=pEnd2 ) *pDst++ = *pSrc2++;

				assert ( pDst<=( dNewCumulative.Begin()+dNewCumulative.GetLength() ) );
				dNewCumulative.Resize ( pDst-dNewCumulative.Begin() );
				dNewCumulative.SwapData ( dCumulativeKList );
			}
		}

		dMergedKillist.Resize ( 0 );
		if ( dCumulativeKList.GetLength() )
		{
			dMergedKillist.Resize ( 1 );
			dMergedKillist.Last().m_pBegin = dCumulativeKList.Begin();
			dMergedKillist.Last().m_iLen = dCumulativeKList.GetLength();
		}

		CSphQueryResult tChunkResult;
		tChunkResult.m_pProfile = pResult->m_pProfile;
		CSphMultiQueryArgs tMultiArgs ( dMergedKillist, tArgs.m_iIndexWeight );
		// storing index in matches tag for finding strings attrs offset later, biased against default zero and segments
		tMultiArgs.m_iTag = tGuard.m_dRamChunks.GetLength()+iChunk+1;
		tMultiArgs.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;
		tMultiArgs.m_bLocalDF = bGotLocalDF;
		tMultiArgs.m_pLocalDocs = pLocalDocs;
		tMultiArgs.m_iTotalDocs = iTotalDocs;

		// we use sorters in both disk chunks and ram chunks, that's why we don't want to move to a new schema before we searched ram chunks
		tMultiArgs.m_bModifySorterSchemas = false;

		if ( !tGuard.m_dDiskChunks[iChunk]->MultiQuery ( pQuery, &tChunkResult, iSorters, ppSorters, tMultiArgs ) )
		{
			// FIXME? maybe handle this more gracefully (convert to a warning)?
			pResult->m_sError = tChunkResult.m_sError;
			return false;
		}

		// check terms inconsistency among disk chunks
		const SmallStringHash_T<CSphQueryResultMeta::WordStat_t> & hDstStats = tChunkResult.m_hWordStats;
		tStat.DumpDiffer ( hDstStats, m_sIndexName.cstr(), pResult->m_sWarning );
		if ( pResult->m_hWordStats.GetLength() )
		{
			pResult->m_hWordStats.IterateStart();
			while ( pResult->m_hWordStats.IterateNext() )
			{
				const CSphQueryResultMeta::WordStat_t * pDstStat = hDstStats ( pResult->m_hWordStats.IterateGetKey() );
				if ( pDstStat )
					pResult->AddStat ( pResult->m_hWordStats.IterateGetKey(), pDstStat->m_iDocs, pDstStat->m_iHits );
			}
		} else
		{
			pResult->m_hWordStats = hDstStats;
		}
		// keep last chunk statistics to check vs rt settings
		if ( iChunk==tGuard.m_dDiskChunks.GetLength()-1 )
			tDiskStat.Set ( hDstStats );
		if ( !iChunk )
			tStat.Set ( hDstStats );

		dDiskStrings[iChunk] = tChunkResult.m_pStrings;
		dDiskMva[iChunk] = tChunkResult.m_pMva;
		if ( tChunkResult.m_bArenaProhibit )
			tMvaArenaFlag.BitSet ( iChunk );
		pResult->m_iBadRows += tChunkResult.m_iBadRows;

		if ( pResult->m_bHasPrediction )
			pResult->m_tStats.Add ( tChunkResult.m_tStats );

		if ( iChunk && tmMaxTimer>0 && sphMicroTimer()>=tmMaxTimer )
		{
			pResult->m_sWarning = "query time exceeded max_query_time";
			break;
		}
	}

	////////////////////
	// search RAM chunk
	////////////////////

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_INIT );

	// select the sorter with max schema
	// uses GetAttrsCount to get working facets (was GetRowSize)
	int iMaxSchemaSize = -1;
	int iMaxSchemaIndex = -1;
	int iMatchPoolSize = 0;
	ARRAY_FOREACH ( i, dSorters )
	{
		iMatchPoolSize += dSorters[i]->m_iMatchCapacity;
		if ( dSorters[i]->GetSchema ()->GetAttrsCount ()>iMaxSchemaSize )
		{
			iMaxSchemaSize = dSorters[i]->GetSchema ()->GetAttrsCount ();
			iMaxSchemaIndex = i;
		}
	}

	if ( iMaxSchemaSize==-1 || iMaxSchemaIndex==-1 )
		return false;

	const ISphSchema & tMaxSorterSchema = *(dSorters[iMaxSchemaIndex]->GetSchema());

	CSphVector< const ISphSchema * > dSorterSchemas;
	SorterSchemas ( dSorters.Begin(), dSorters.GetLength(), iMaxSchemaIndex, dSorterSchemas );

	// setup calculations and result schema
	CSphQueryContext tCtx ( *pQuery );
	tCtx.m_pProfile = pProfiler;
	if ( !tCtx.SetupCalc ( pResult, tMaxSorterSchema, m_tSchema, nullptr, false, dSorterSchemas ) )
		return false;

	tCtx.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;
	tCtx.m_pLocalDocs = pLocalDocs;
	tCtx.m_iTotalDocs = iTotalDocs;

	// setup search terms
	RtQwordSetup_t tTermSetup ( tGuard );
	tTermSetup.SetDict ( pDict );
	tTermSetup.m_pIndex = this;
	tTermSetup.m_eDocinfo = m_tSettings.m_eDocinfo;
	tTermSetup.m_iDynamicRowitems = tMaxSorterSchema.GetDynamicSize();
	if ( pQuery->m_uMaxQueryMsec>0 )
		tTermSetup.m_iMaxTimer = sphMicroTimer() + pQuery->m_uMaxQueryMsec*1000; // max_query_time
	tTermSetup.m_pWarning = &pResult->m_sWarning;
	tTermSetup.SetSegment ( -1 );
	tTermSetup.m_pCtx = &tCtx;

	// setup prediction constrain
	CSphQueryStats tQueryStats;
	int64_t iNanoBudget = (int64_t)(pQuery->m_iMaxPredictedMsec) * 1000000; // from milliseconds to nanoseconds
	tQueryStats.m_pNanoBudget = &iNanoBudget;
	if ( pResult->m_bHasPrediction )
		tTermSetup.m_pStats = &tQueryStats;

	// bind weights
	tCtx.BindWeights ( pQuery, m_tSchema, pResult->m_sWarning );

	CSphVector<BYTE> dFiltered;
	const BYTE * sModifiedQuery = (BYTE *)pQuery->m_sQuery.cstr();

	ISphFieldFilterRefPtr_c pFieldFilter;
	if ( m_pFieldFilter )
	{
		pFieldFilter = m_pFieldFilter->Clone();
		if ( pFieldFilter && pFieldFilter->Apply ( sModifiedQuery, strlen ( (char*)sModifiedQuery ), dFiltered, true ) )
			sModifiedQuery = dFiltered.Begin();
	}

	// parse query
	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_PARSE );

	XQQuery_t tParsed;
	// FIXME!!! provide segments list instead index to tTermSetup.m_pIndex

	const QueryParser_i * pQueryParser = pQuery->m_pQueryParser;
	assert ( pQueryParser );

	CSphScopedPtr<ISphRanker> pRanker ( nullptr );
	CSphScopedPayload tPayloads;

	// FIXME!!! add proper
	// - qcache invalidation after INSERT \ DELETE \ UPDATE and for plain index afte UPDATE #256
	// - qcache duplicates removal from killed document at segment #263
	tCtx.m_bSkipQCache = true;

	// no need to create ranker, etc if there's no query
	if ( !pQueryParser->IsFullscan(*pQuery) )
	{
		// OPTIMIZE! make a lightweight clone here? and/or remove double clone?
		ISphTokenizerRefPtr_c pQueryTokenizerJson { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
		sphSetupQueryTokenizer ( pQueryTokenizerJson, IsStarDict (), m_tSettings.m_bIndexExactWords, true );

		if ( !pQueryParser->ParseQuery ( tParsed, (const char *)sModifiedQuery, pQuery, pQueryTokenizer, pQueryTokenizerJson, &m_tSchema, pDict, m_tSettings ) )
		{
			pResult->m_sError = tParsed.m_sParseError;
			return false;
		}

		if ( !tParsed.m_sParseWarning.IsEmpty() )
			pResult->m_sWarning = tParsed.m_sParseWarning;

		// transform query if needed (quorum transform, etc.)
		if ( pProfiler )
			pProfiler->Switch ( SPH_QSTATE_TRANSFORMS );

		// FIXME!!! provide segments list instead index
		sphTransformExtendedQuery ( &tParsed.m_pRoot, m_tSettings, pQuery->m_bSimplify, this );

		int iExpandKeywords = ExpandKeywords ( m_iExpandKeywords, pQuery->m_eExpandKeywords, m_tSettings );
		if ( iExpandKeywords!=KWE_DISABLED )
		{
			tParsed.m_pRoot = sphQueryExpandKeywords ( tParsed.m_pRoot, m_tSettings, iExpandKeywords );
			tParsed.m_pRoot->Check ( true );
		}

		// this should be after keyword expansion
		if ( m_tSettings.m_uAotFilterMask )
			TransformAotFilter ( tParsed.m_pRoot, pDict->GetWordforms(), m_tSettings );

		// expanding prefix in word dictionary case
		if ( m_bKeywordDict && IsStarDict() )
		{
			ExpansionContext_t tExpCtx;
			tExpCtx.m_pWordlist = this;
			tExpCtx.m_pBuf = NULL;
			tExpCtx.m_pResult = pResult;
			tExpCtx.m_iMinPrefixLen = m_tSettings.m_iMinPrefixLen;
			tExpCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
			tExpCtx.m_iExpansionLimit = m_iExpansionLimit;
			tExpCtx.m_bHasMorphology = m_pDict->HasMorphology();
			tExpCtx.m_bMergeSingles = ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_INLINE && ( pQuery->m_uDebugFlags & QUERY_DEBUG_NO_PAYLOAD )==0 );
			tExpCtx.m_pPayloads = &tPayloads;
			tExpCtx.m_pIndexData = &tGuard.m_dRamChunks;

			tParsed.m_pRoot = sphExpandXQNode ( tParsed.m_pRoot, tExpCtx );
		}

		if ( !sphCheckQueryHeight ( tParsed.m_pRoot, pResult->m_sError ) )
			return false;

		// set zonespanlist settings
		tParsed.m_bNeedSZlist = pQuery->m_bZSlist;

		// setup query
		// must happen before index-level reject, in order to build proper keyword stats
		pRanker = sphCreateRanker ( tParsed, pQuery, pResult, tTermSetup, tCtx, tMaxSorterSchema );
		if ( !pRanker.Ptr() )
			return false;

		tCtx.SetupExtraData ( pRanker.Ptr(), iSorters==1 ? ppSorters[0] : NULL );

		// check terms inconsistency disk chunks vs rt vs previous indexes
		tDiskStat.DumpDiffer ( pResult->m_hWordStats, m_sIndexName.cstr(), pResult->m_sWarning );
		tStat.DumpDiffer ( pResult->m_hWordStats, m_sIndexName.cstr(), pResult->m_sWarning );

		pRanker->ExtraData ( EXTRA_SET_POOL_CAPACITY, (void**)&iMatchPoolSize );

		// check for the possible integer overflow in m_dPool.Resize
		int64_t iPoolSize = 0;
		if ( pRanker->ExtraData ( EXTRA_GET_POOL_SIZE, (void**)&iPoolSize ) && iPoolSize>INT_MAX )
		{
			pResult->m_sError.SetSprintf ( "ranking factors pool too big (%d Mb), reduce max_matches", (int)( iPoolSize/1024/1024 ) );
			return false;
		}
	}

	// empty index, empty result
	if ( !tGuard.m_dRamChunks.GetLength() && !tGuard.m_dDiskChunks.GetLength() )
	{
		for ( auto i : dSorters )
			TransformSorterSchema ( i, tGuard, dDiskMva, dDiskStrings, tMvaArenaFlag );

		pResult->m_iQueryTime = 0;
		return true;
	}

	// probably redundant, but just in case
	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_INIT );

	// search segments no looking to max_query_time
	// FIXME!!! move searching at segments before disk chunks as result set is safe with kill-lists
	if ( tGuard.m_dRamChunks.GetLength() )
	{
		// setup filters
		// FIXME! setup filters MVA pool
		bool bFullscan = pQuery->m_pQueryParser->IsFullscan ( *pQuery ) || pQuery->m_pQueryParser->IsFullscan ( tParsed );
		auto dKillList = KillListVector ();
		CreateFilterContext_t tFlx;
		tFlx.m_pFilters = &pQuery->m_dFilters;
		tFlx.m_pFilterTree = &pQuery->m_dFilterTree;
		tFlx.m_pKillList = &dKillList;
		tFlx.m_pSchema = &tMaxSorterSchema;
		tFlx.m_eCollation = pQuery->m_eCollation;
		tFlx.m_bScan = bFullscan;

		if ( !tCtx.CreateFilters ( tFlx, pResult->m_sError, pResult->m_sWarning ) )
			return false;

		// FIXME! OPTIMIZE! check if we can early reject the whole index

		// setup lookup
		// do pre-filter lookup as needed
		// do pre-sort lookup in all cases
		// post-sort lookup is complicated (because of many segments)
		// pre-sort lookup is cheap now anyway, and almost always anyway
		// (except maybe by stupid relevance-sorting-only benchmarks!!)
		tCtx.m_bLookupFilter = ( pQuery->m_dFilters.GetLength() || tCtx.m_dCalcFilter.GetLength() );
		tCtx.m_bLookupSort = true;

		// FIXME! setup overrides

		// do searching
		bool bRandomize = dSorters[0]->m_bRandomize;
		int iCutoff = pQuery->m_iCutoff;
		if ( iCutoff<=0 )
			iCutoff = -1;

		if ( bFullscan )
		{
			if ( pProfiler )
				pProfiler->Switch ( SPH_QSTATE_FULLSCAN );

			// full scan
			// FIXME? OPTIMIZE? add shortcuts here too?
			CSphMatch tMatch;
			tMatch.Reset ( tMaxSorterSchema.GetDynamicSize() );
			tMatch.m_iWeight = tArgs.m_iIndexWeight;

			ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
			{
				// set string pool for string on_sort expression fix up
				tCtx.SetStringPool ( tGuard.m_dRamChunks[iSeg]->m_dStrings.Begin() );
				tCtx.SetMVAPool ( tGuard.m_dRamChunks[iSeg]->m_dMvas.Begin(), false );
				ARRAY_FOREACH ( i, dSorters )
				{
					dSorters[i]->SetStringPool ( tGuard.m_dRamChunks[iSeg]->m_dStrings.Begin() );
					dSorters[i]->SetMVAPool ( tGuard.m_dRamChunks[iSeg]->m_dMvas.Begin(), false );
				}

				RtRowIterator_t tIt ( tGuard.m_dRamChunks[iSeg], m_iStride, false, NULL, tGuard.m_dKill[iSeg]->m_dKilled );
				while (true)
				{
					const CSphRowitem * pRow = tIt.GetNextAliveRow();
					if ( !pRow )
						break;

					tMatch.m_uDocID = DOCINFO2ID(pRow);
					tMatch.m_pStatic = DOCINFO2ATTRS(pRow); // FIXME! overrides

					tCtx.CalcFilter ( tMatch );
					if ( tCtx.m_pFilter && !tCtx.m_pFilter->Eval ( tMatch ) )
					{
						tCtx.FreeDataFilter ( tMatch );
						continue;
					}

					if ( bRandomize )
						tMatch.m_iWeight = ( sphRand() & 0xffff ) * tArgs.m_iIndexWeight;

					tCtx.CalcSort ( tMatch );

					// storing segment in matches tag for finding strings attrs offset later, biased against default zero
					tMatch.m_iTag = iSeg+1;

					bool bNewMatch = false;
					ARRAY_FOREACH ( iSorter, dSorters )
						bNewMatch |= dSorters[iSorter]->Push ( tMatch );

					// stringptr expressions should be duplicated (or taken over) at this point
					tCtx.FreeDataFilter ( tMatch );
					tCtx.FreeDataSort ( tMatch );

					// handle cutoff
					if ( bNewMatch )
						if ( --iCutoff==0 )
							break;

					// handle timer
					if ( tmMaxTimer && sphMicroTimer()>=tmMaxTimer )
					{
						pResult->m_sWarning = "query time exceeded max_query_time";
						iSeg = tGuard.m_dRamChunks.GetLength() - 1;	// outer break
						break;
					}
				}

				if ( iCutoff==0 )
					break;
			}

		} else
		{
			// query matching
			ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
			{
				if ( pProfiler )
					pProfiler->Switch ( SPH_QSTATE_INIT_SEGMENT );

				tTermSetup.SetSegment ( iSeg );
				pRanker->Reset ( tTermSetup );

				// for lookups to work
				tCtx.m_pIndexData = tGuard.m_dRamChunks[iSeg];

				// set string pool for string on_sort expression fix up
				tCtx.SetStringPool ( tGuard.m_dRamChunks[iSeg]->m_dStrings.Begin() );
				tCtx.SetMVAPool ( tGuard.m_dRamChunks[iSeg]->m_dMvas.Begin(), false );
				ARRAY_FOREACH ( i, dSorters )
				{
					dSorters[i]->SetStringPool ( tGuard.m_dRamChunks[iSeg]->m_dStrings.Begin() );
					dSorters[i]->SetMVAPool ( tGuard.m_dRamChunks[iSeg]->m_dMvas.Begin(), false );
				}
				PoolPtrs_t tMva;
				tMva.m_pMva = tGuard.m_dRamChunks[iSeg]->m_dMvas.Begin();
				tMva.m_bArenaProhibit = false;
				pRanker->ExtraData ( EXTRA_SET_MVAPOOL, (void**)&tMva );
				pRanker->ExtraData ( EXTRA_SET_STRINGPOOL, (void**)tGuard.m_dRamChunks[iSeg]->m_dStrings.Begin() );

				CSphMatch * pMatch = pRanker->GetMatchesBuffer();
				while (true)
				{
					// ranker does profile switches internally in GetMatches()
					int iMatches = pRanker->GetMatches();
					if ( iMatches<=0 )
						break;

					if ( pProfiler )
						pProfiler->Switch ( SPH_QSTATE_SORT );
					for ( int i=0; i<iMatches; i++ )
					{
						if ( tCtx.m_bLookupSort )
						{
							// tricky bit
							// that kills two birds with one stone, sort of
							// first, broken indexes MIGHT yield nonexistent docids at this point
							// second, query cache ranker WILL return nonexistent docids (from other segments)
							// because the cached query only stores docids, not segment ids (which change all the time anyway)
							// to catch broken indexes or other bugs in debug, we have that assert
							// but release builds will simply ignore nonexistent docids, whatever the reason they do not exist
							assert ( m_iStride==( DOCINFO_IDSIZE + m_tSchema.GetRowSize() ) );
							const CSphRowitem * pRow = FindDocinfo ( tGuard.m_dRamChunks[iSeg], pMatch[i].m_uDocID, m_iStride );
							assert ( pRanker->IsCache() || pRow );
							if ( !pRow )
							{
								tCtx.m_iBadRows++;
								continue;
							}
							CopyDocinfo ( pMatch[i], pRow );
						}

						pMatch[i].m_iWeight *= tArgs.m_iIndexWeight;
						if ( bRandomize )
							pMatch[i].m_iWeight = ( sphRand() & 0xffff ) * tArgs.m_iIndexWeight;

						tCtx.CalcSort ( pMatch[i] );

						if ( tCtx.m_pWeightFilter && !tCtx.m_pWeightFilter->Eval ( pMatch[i] ) )
						{
							tCtx.FreeDataSort ( pMatch[i] );
							continue;
						}

						// storing segment in matches tag for finding strings attrs offset later, biased against default zero
						pMatch[i].m_iTag = iSeg+1;

						bool bNewMatch = false;
						ARRAY_FOREACH ( iSorter, dSorters )
						{
							bNewMatch |= dSorters[iSorter]->Push ( pMatch[i] );

							if ( tCtx.m_uPackedFactorFlags & SPH_FACTOR_ENABLE )
							{
								pRanker->ExtraData ( EXTRA_SET_MATCHPUSHED, (void**)&(dSorters[iSorter]->m_iJustPushed) );
								pRanker->ExtraData ( EXTRA_SET_MATCHPOPPED, (void**)&(dSorters[iSorter]->m_dJustPopped) );
							}
						}

						// stringptr expressions should be duplicated (or taken over) at this point
						tCtx.FreeDataSort ( pMatch[i] );

						if ( bNewMatch )
							if ( --iCutoff==0 )
								break;
					}

					if ( iCutoff==0 )
					{
						iSeg = tGuard.m_dRamChunks.GetLength();
						break;
					}
				}
			}
		}
	}

	// do final expression calculations
	if ( tCtx.m_dCalcFinal.GetLength () )
	{
		const int iSegmentsTotal = tGuard.m_dRamChunks.GetLength ();

		// at 0 pass processor also fills bitmask of segments these has matches at sorter
		// then skip sorter processing for these 'empty' segments
		SphRtFinalMatchCalc_t tFinal ( iSegmentsTotal, tCtx );

		ARRAY_FOREACH_COND ( iSeg, tGuard.m_dRamChunks, tFinal.HasSegments() )
		{
			if ( !tFinal.NextSegment ( iSeg ) )
				continue;

			// set string pool for string on_sort expression fix up
			tCtx.SetStringPool ( tGuard.m_dRamChunks[iSeg]->m_dStrings.Begin() );
			tCtx.SetMVAPool ( tGuard.m_dRamChunks[iSeg]->m_dMvas.Begin(), false );

			for ( int iSorter = 0; iSorter<iSorters; iSorter++ )
			{
				ISphMatchSorter * pTop = ppSorters[iSorter];
				if ( pTop )
					pTop->Finalize ( tFinal, false );
			}
		}
	}


	//////////////////////
	// copying match's attributes to external storage in result set
	//////////////////////

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_FINALIZE );

	if ( pRanker.Ptr() )
		pRanker->FinalizeCache ( tMaxSorterSchema );

	MEMORY ( MEM_RT_RES_STRINGS );

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_DYNAMIC );

	// create new standalone schema for sorters (independent of any external indexes/pools/storages)
	// modify matches inside the sorters to work with the new schema
	for ( auto i : dSorters )
		TransformSorterSchema ( i, tGuard, dDiskMva, dDiskStrings, tMvaArenaFlag );

	if ( pProfiler )
		pProfiler->Switch ( eOldState );

	if ( pResult->m_bHasPrediction )
		pResult->m_tStats.Add ( tQueryStats );

	// query timer
	pResult->m_iQueryTime = int ( ( sphMicroTimer()-tmQueryStart )/1000 );
	return true;
}

bool RtIndex_t::MultiQueryEx ( int iQueries, const CSphQuery * ppQueries, CSphQueryResult ** ppResults,
								ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	// FIXME! OPTIMIZE! implement common subtree cache here
	bool bResult = false;
	for ( int i=0; i<iQueries; ++i )
		if ( MultiQuery ( &ppQueries[i], ppResults[i], 1, &ppSorters[i], tArgs ) )
			bResult = true;
		else
			ppResults[i]->m_iMultiplier = -1;

	return bResult;
}


void RtIndex_t::AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, CSphDict * pDict, bool bGetStats, int iQpos, RtQword_t * pQueryWord, CSphVector <CSphKeywordInfo> & dKeywords, const SphChunkGuard_t & tGuard ) const
{
	assert ( !bGetStats || pQueryWord );

	SphWordID_t iWord = pDict->GetWordID ( sWord );
	if ( !iWord )
		return;

	if ( bGetStats )
	{
		pQueryWord->Reset();
		pQueryWord->m_uWordID = iWord;
		pQueryWord->m_sWord = (const char *)sTokenized;
		pQueryWord->m_sDictWord = (const char *)sWord;
		ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
			RtQwordSetupSegment ( pQueryWord, tGuard.m_dRamChunks[iSeg], false, m_bKeywordDict, m_iWordsCheckpoint, tGuard.m_dKill[iSeg]->m_dKilled, m_tSettings );
	}

	CSphKeywordInfo & tInfo = dKeywords.Add();
	tInfo.m_sTokenized = (const char *)sTokenized;
	tInfo.m_sNormalized = (const char*)sWord;
	tInfo.m_iDocs = bGetStats ? pQueryWord->m_iDocs : 0;
	tInfo.m_iHits = bGetStats ? pQueryWord->m_iHits : 0;
	tInfo.m_iQpos = iQpos;

	RemoveDictSpecials ( tInfo.m_sNormalized );
}


struct CSphRtQueryFilter : public ISphQueryFilter, public ISphNoncopyable
{
	const RtIndex_t *	m_pIndex;
	RtQword_t *			m_pQword;
	bool				m_bGetStats = false;
	const SphChunkGuard_t & m_tGuard;

	CSphRtQueryFilter ( const RtIndex_t * pIndex, RtQword_t * pQword, const SphChunkGuard_t & tGuard )
		: m_pIndex ( pIndex )
		, m_pQword ( pQword )
		, m_tGuard ( tGuard )
	{}

	void AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, int iQpos, CSphVector <CSphKeywordInfo> & dKeywords ) final
	{
		assert ( m_pIndex && m_pQword );
		m_pIndex->AddKeywordStats ( sWord, sTokenized, m_pDict, m_tFoldSettings.m_bStats, iQpos, m_pQword, dKeywords, m_tGuard );
	}
};

static void HashKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, SmallStringHash_T<CSphKeywordInfo> & hKeywords )
{
	for ( CSphKeywordInfo & tSrc : dKeywords )
	{
		CSphKeywordInfo & tDst = hKeywords.AddUnique ( tSrc.m_sNormalized );
		tDst.m_sTokenized = std::move ( tSrc.m_sTokenized );
		tDst.m_sNormalized = std::move ( tSrc.m_sNormalized );
		tDst.m_iQpos = tSrc.m_iQpos;
		tDst.m_iDocs += tSrc.m_iDocs;
		tDst.m_iHits += tSrc.m_iHits;
	}
}

bool RtIndex_t::DoGetKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, const char * sQuery, const GetKeywordsSettings_t & tSettings, bool bFillOnly, CSphString * pError, const SphChunkGuard_t & tGuard ) const
{
	if ( !bFillOnly )
		dKeywords.Resize ( 0 );

	if ( ( bFillOnly && !dKeywords.GetLength() ) || ( !bFillOnly && ( !sQuery || !sQuery[0] ) ) )
		return true;

	RtQword_t tQword;

	ISphTokenizerRefPtr_c pTokenizer { m_pTokenizer->Clone ( SPH_CLONE_INDEX ) };
	pTokenizer->EnableTokenizedMultiformTracking ();

	// need to support '*' and '=' but not the other specials
	// so m_pQueryTokenizer does not work for us, gotta clone and setup one manually
	CSphDictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( IsStarDict () )
	{
		if ( m_bKeywordDict )
			SetupStarDict ( pDict, pTokenizer );
		else
			pTokenizer->AddPlainChar ( '*' );
	}

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pTokenizer, false );

	// FIXME!!! missed bigram, FieldFilter

	if ( !bFillOnly )
	{
		ExpansionContext_t tExpCtx;

		// query defined options
		tExpCtx.m_iExpansionLimit = tSettings.m_iExpansionLimit ? tSettings.m_iExpansionLimit : m_iExpansionLimit;
		bool bExpandWildcards = ( m_bKeywordDict && IsStarDict() && !tSettings.m_bFoldWildcards );

		CSphRtQueryFilter tAotFilter ( this, &tQword, tGuard );
		tAotFilter.m_pTokenizer = pTokenizer;
		tAotFilter.m_pDict = pDict;
		tAotFilter.m_pSettings = &m_tSettings;
		tAotFilter.m_tFoldSettings = tSettings;
		tAotFilter.m_tFoldSettings.m_bFoldWildcards = !bExpandWildcards;

		tExpCtx.m_pWordlist = this;
		tExpCtx.m_iMinPrefixLen = m_tSettings.m_iMinPrefixLen;
		tExpCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
		tExpCtx.m_bHasMorphology = m_pDict->HasMorphology();
		tExpCtx.m_bMergeSingles = false;
		tExpCtx.m_pIndexData = &tGuard.m_dRamChunks;

		pTokenizer->SetBuffer ( (BYTE *)sQuery, strlen ( sQuery ) );
		tAotFilter.GetKeywords ( dKeywords, tExpCtx );
	} else
	{
		BYTE sWord[SPH_MAX_KEYWORD_LEN];

		ARRAY_FOREACH ( i, dKeywords )
		{
			CSphKeywordInfo & tInfo = dKeywords[i];
			int iLen = tInfo.m_sTokenized.Length();
			memcpy ( sWord, tInfo.m_sTokenized.cstr(), iLen );
			sWord[iLen] = '\0';

			SphWordID_t iWord = pDict->GetWordID ( sWord );
			if ( iWord )
			{
				tQword.Reset();
				tQword.m_uWordID = iWord;
				tQword.m_sWord = tInfo.m_sTokenized;
				tQword.m_sDictWord = (const char *)sWord;
				ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
					RtQwordSetupSegment ( &tQword, tGuard.m_dRamChunks[iSeg], false, m_bKeywordDict, m_iWordsCheckpoint, tGuard.m_dKill[iSeg]->m_dKilled, m_tSettings );

				tInfo.m_iDocs += tQword.m_iDocs;
				tInfo.m_iHits += tQword.m_iHits;
			}
		}
	}

	// get stats from disk chunks too
	if ( !tSettings.m_bStats )
		return true;

	if ( bFillOnly )
	{
		ARRAY_FOREACH ( iChunk, tGuard.m_dDiskChunks )
			tGuard.m_dDiskChunks[iChunk]->FillKeywords ( dKeywords );
	} else
	{
		// bigram and expanded might differs need to merge infos
		CSphVector<CSphKeywordInfo> dChunkKeywords;
		SmallStringHash_T<CSphKeywordInfo> hKeywords;
		ARRAY_FOREACH ( iChunk, tGuard.m_dDiskChunks )
		{
			tGuard.m_dDiskChunks[iChunk]->GetKeywords ( dChunkKeywords, sQuery, tSettings, pError );
			HashKeywords ( dChunkKeywords, hKeywords );
			dChunkKeywords.Resize ( 0 );
		}

		if ( hKeywords.GetLength() )
		{
			// merge keywords from RAM parts with disk keywords into hash
			HashKeywords ( dKeywords, hKeywords );
			dKeywords.Resize ( 0 );
			dKeywords.Reserve ( hKeywords.GetLength() );

			hKeywords.IterateStart();
			while ( hKeywords.IterateNext() )
			{
				const CSphKeywordInfo & tSrc = hKeywords.IterateGet();
				dKeywords.Add ( tSrc );
			}
			sphSort ( dKeywords.Begin(), dKeywords.GetLength(), bind ( &CSphKeywordInfo::m_iQpos ) );
		}
	}

	return true;
}


bool RtIndex_t::GetKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, const char * sQuery, const GetKeywordsSettings_t & tSettings, CSphString * pError ) const
{
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );
	bool bGot = DoGetKeywords ( dKeywords, sQuery, tSettings, false, pError, tGuard );
	return bGot;
}


bool RtIndex_t::FillKeywords ( CSphVector<CSphKeywordInfo> & dKeywords ) const
{
	GetKeywordsSettings_t tSettings;
	tSettings.m_bStats = true;
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );
	bool bGot = DoGetKeywords ( dKeywords, NULL, tSettings, true, NULL, tGuard );
	return bGot;
}


static const RtSegment_t * UpdateFindSegment ( const SphChunkGuard_t & tGuard, const CSphRowitem ** ppRow, SphDocID_t uDocID )
{
	assert ( ppRow && ( ( *ppRow!=NULL ) ^ ( uDocID!=0 ) ) );

	const CSphRowitem * pRow = *ppRow;
	*ppRow = NULL;

	if ( uDocID )
	{
		ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
		{
			bool bKilled = ( tGuard.m_dKill[i]->m_dKilled.BinarySearch ( uDocID )!=NULL );
			if ( bKilled )
				continue;

			pRow = const_cast<CSphRowitem *> ( tGuard.m_dRamChunks[i]->FindRow ( uDocID ) );
			if ( !pRow )
				continue;

			*ppRow = pRow;
			return tGuard.m_dRamChunks[i];
		}
	} else
	{
		ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
		{
			const CSphTightVector<CSphRowitem> & dRows = tGuard.m_dRamChunks[i]->m_dRows;
			if ( dRows.Begin()<=pRow && pRow<dRows.Begin()+ dRows.GetLength() )
			{
				*ppRow = pRow;
				return tGuard.m_dRamChunks[i];
			}
		}
	}

	return NULL;
}


// FIXME! might be inconsistent in case disk chunk update fails
int RtIndex_t::UpdateAttributes ( const CSphAttrUpdate & tUpd, int iIndex, CSphString & sError, CSphString & sWarning )
{
	assert ( tUpd.m_dDocids.GetLength()==tUpd.m_dRows.GetLength() );
	assert ( tUpd.m_dDocids.GetLength()==tUpd.m_dRowOffset.GetLength() );
	int iRows = tUpd.m_dDocids.GetLength();
	bool bHasMva = false;

	if ( !iRows )
		return 0;

	// remap update schema to index schema
	int iUpdLen = tUpd.m_dAttrs.GetLength();
	CSphVector<CSphAttrLocator> dLocators ( iUpdLen );
	CSphBitvec dBigints ( iUpdLen );
	CSphBitvec dDoubles ( iUpdLen );
	CSphBitvec dJsonFields ( iUpdLen );
	CSphBitvec dBigint2Float ( iUpdLen );
	CSphBitvec dFloat2Bigint ( iUpdLen );
	CSphVector < CSphRefcountedPtr<ISphExpr> > dExpr ( iUpdLen );
	dLocators.ZeroMem ();

	uint64_t uDst64 = 0;
	ARRAY_FOREACH ( i, tUpd.m_dAttrs )
	{
		int iIdx = m_tSchema.GetAttrIndex ( tUpd.m_dAttrs[i] );

		if ( iIdx<0 )
		{
			CSphString sJsonCol, sJsonKey;
			if ( sphJsonNameSplit ( tUpd.m_dAttrs[i], &sJsonCol, &sJsonKey ) )
			{
				iIdx = m_tSchema.GetAttrIndex ( sJsonCol.cstr() );
				if ( iIdx>=0 )
					dExpr[i] = sphExprParse ( tUpd.m_dAttrs[i], m_tSchema, NULL, NULL, sError, NULL );
			}
		}

		if ( iIdx>=0 )
		{
			// forbid updates on non-int columns
			const CSphColumnInfo & tCol = m_tSchema.GetAttr(iIdx);
			if ( !( tCol.m_eAttrType==SPH_ATTR_BOOL || tCol.m_eAttrType==SPH_ATTR_INTEGER || tCol.m_eAttrType==SPH_ATTR_TIMESTAMP
				|| tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET
				|| tCol.m_eAttrType==SPH_ATTR_BIGINT || tCol.m_eAttrType==SPH_ATTR_FLOAT || tCol.m_eAttrType==SPH_ATTR_JSON ))
			{
				sError.SetSprintf ( "attribute '%s' can not be updated "
					"(must be boolean, integer, bigint, float, timestamp, MVA or JSON)",
					tUpd.m_dAttrs[i] );
				return -1;
			}

			bool bSrcMva = ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET );
			bool bDstMva = ( tUpd.m_dTypes[i]==SPH_ATTR_UINT32SET || tUpd.m_dTypes[i]==SPH_ATTR_INT64SET );
			if ( bSrcMva!=bDstMva )
			{
				sError.SetSprintf ( "attribute '%s' MVA flag mismatch", tUpd.m_dAttrs[i] );
				return -1;
			}

			if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET && tUpd.m_dTypes[i]==SPH_ATTR_INT64SET )
			{
				sError.SetSprintf ( "attribute '%s' MVA bits (dst=%d, src=%d) mismatch", tUpd.m_dAttrs[i],
					tCol.m_eAttrType, tUpd.m_dTypes[i] );
				return -1;
			}

			if ( tCol.m_eAttrType==SPH_ATTR_INT64SET )
				uDst64 |= ( U64C(1)<<i );

			if ( tCol.m_eAttrType==SPH_ATTR_FLOAT )
			{
				if ( tUpd.m_dTypes[i]==SPH_ATTR_BIGINT )
					dBigint2Float.BitSet(i);
			} else if ( tCol.m_eAttrType==SPH_ATTR_JSON )
				dJsonFields.BitSet(i);
			else if ( tCol.m_eAttrType==SPH_ATTR_BIGINT )
			{
				if ( tUpd.m_dTypes[i]==SPH_ATTR_FLOAT )
					dFloat2Bigint.BitSet(i);
			}

			dLocators[i] = tCol.m_tLocator;
			bHasMva |= ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET );
		} else if ( tUpd.m_bIgnoreNonexistent )
		{
			continue;
		} else
		{
			sError.SetSprintf ( "attribute '%s' not found", tUpd.m_dAttrs[i] );
			return -1;
		}

		// this is a hack
		// Query parser tries to detect an attribute type. And this is wrong because, we should
		// take attribute type from schema. Probably we'll rewrite updates in future but
		// for now this fix just works.
		// Fixes cases like UPDATE float_attr=1 WHERE id=1;
		assert ( iIdx>=0 );
		if ( tUpd.m_dTypes[i]==SPH_ATTR_INTEGER && m_tSchema.GetAttr(iIdx).m_eAttrType==SPH_ATTR_FLOAT )
		{
			const_cast<CSphAttrUpdate &>(tUpd).m_dTypes[i] = SPH_ATTR_FLOAT;
			const_cast<CSphAttrUpdate &>(tUpd).m_dPool[i] = sphF2DW ( (float)tUpd.m_dPool[i] );
		}

		if ( tUpd.m_dTypes[i]==SPH_ATTR_BIGINT )
			dBigints.BitSet(i);
		else if ( tUpd.m_dTypes[i]==SPH_ATTR_FLOAT )
			dDoubles.BitSet(i);
	}

	// check if we are empty
	if ( !m_dRamChunks.GetLength() && !m_dDiskChunks.GetLength() )
	{
		return true;
	}

	// FIXME!!! grab Writer lock to prevent segments retirement during commit(merge)
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	// do the update
	int iUpdated = 0;
	DWORD uUpdateMask = 0;
	int iJsonWarnings = 0;

	// bRaw do only one pass as it has pointers to actual data at segments
	// MVA && bRaw should find appropriate segment to update storage there

	int iFirst = ( iIndex<0 ) ? 0 : iIndex;
	int iLast = ( iIndex<0 ) ? iRows : iIndex+1;

	// first pass, if needed
	if ( tUpd.m_bStrict )
	{
		for ( int iUpd=iFirst; iUpd<iLast; iUpd++ )
		{
			const CSphRowitem * pRow = tUpd.m_dRows[iUpd];
			SphDocID_t uDocid = tUpd.m_dDocids[iUpd];

			const RtSegment_t * pSegment = UpdateFindSegment ( tGuard, &pRow, uDocid );
			if ( !pRow )
				continue;

			assert ( !uDocid || DOCINFO2ID(pRow)==uDocid );
			pRow = DOCINFO2ATTRS(pRow);

			int iPos = tUpd.m_dRowOffset[iUpd];
			ARRAY_FOREACH ( iCol, tUpd.m_dAttrs )
			{
				if ( !dJsonFields.BitGet ( iCol ) )
					continue;

				ESphJsonType eType = dDoubles.BitGet ( iCol )
					? JSON_DOUBLE
					: ( dBigints.BitGet ( iCol ) ? JSON_INT64 : JSON_INT32 );

				SphAttr_t uValue = dDoubles.BitGet ( iCol )
					? sphD2QW ( (double)sphDW2F ( tUpd.m_dPool[iPos] ) )
					: dBigints.BitGet ( iCol ) ? MVA_UPSIZE ( &tUpd.m_dPool[iPos] ) : tUpd.m_dPool[iPos];

				if ( !sphJsonInplaceUpdate ( eType, uValue, dExpr[iCol], (BYTE *)pSegment->m_dStrings.Begin(), pRow, false ) )
				{
					sError.SetSprintf ( "attribute '%s' can not be updated (not found or incompatible types)", tUpd.m_dAttrs[iCol] );
					return -1;
				}

				iPos += dBigints.BitGet ( iCol ) ? 2 : 1;
			}
		}
	}

	CSphVector<SphDocID_t> dKilled;
	m_tKlist.Flush ( dKilled );

	for ( int iUpd=iFirst; iUpd<iLast; iUpd++ )
	{
		// search segments first
		bool bUpdated = false;
		while (true)
		{
			const CSphRowitem * pRow = tUpd.m_dRows[iUpd];
			SphDocID_t uDocid = tUpd.m_dDocids[iUpd];

			RtSegment_t * pSegment = const_cast<RtSegment_t *> ( UpdateFindSegment ( tGuard, &pRow, uDocid ) );
			if ( !pRow )
				break;

			assert ( pSegment );
			assert ( !uDocid || ( DOCINFO2ID(pRow)==uDocid ) );
			pRow = DOCINFO2ATTRS(pRow);

			int iPos = tUpd.m_dRowOffset[iUpd];
			ARRAY_FOREACH ( iCol, tUpd.m_dAttrs )
			{
				if ( dJsonFields.BitGet ( iCol ) )
				{
					ESphJsonType eType = dDoubles.BitGet ( iCol )
						? JSON_DOUBLE
						: ( dBigints.BitGet ( iCol ) ? JSON_INT64 : JSON_INT32 );

					SphAttr_t uValue = dDoubles.BitGet ( iCol )
						? sphD2QW ( (double)sphDW2F ( tUpd.m_dPool[iPos] ) )
						: dBigints.BitGet ( iCol ) ? MVA_UPSIZE ( &tUpd.m_dPool[iPos] ) : tUpd.m_dPool[iPos];

					if ( sphJsonInplaceUpdate ( eType, uValue, dExpr[iCol], pSegment->m_dStrings.Begin(), pRow, true ) )
					{
						bUpdated = true;
						uUpdateMask |= ATTRS_STRINGS_UPDATED;

					} else
						iJsonWarnings++;

					iPos += dBigints.BitGet ( iCol ) ? 2 : 1;
					continue;
				}

				if ( !( tUpd.m_dTypes[iCol]==SPH_ATTR_UINT32SET || tUpd.m_dTypes[iCol]==SPH_ATTR_INT64SET ) )
				{
					// plain update
					bUpdated = true;
					uUpdateMask |= ATTRS_UPDATED;

					SphAttr_t uValue = dBigints.BitGet ( iCol ) ? MVA_UPSIZE ( &tUpd.m_dPool[iPos] ) : tUpd.m_dPool[iPos];
					if ( dBigint2Float.BitGet(iCol) ) // handle bigint(-1) -> float attr updates
						uValue = sphF2DW ( float((int64_t)uValue) );
					else if ( dFloat2Bigint.BitGet(iCol) ) // handle float(1.0) -> bigint attr updates
						uValue = (int64_t)sphDW2F((DWORD)uValue);

					sphSetRowAttr ( const_cast<CSphRowitem *>( pRow ), dLocators[iCol], uValue );

					iPos += dBigints.BitGet ( iCol ) ? 2 : 1;
				} else
				{
					const DWORD * pSrc = tUpd.m_dPool.Begin()+iPos;
					DWORD iLen = *pSrc;
					iPos += iLen+1;

					// MVA update
					bUpdated = true;
					uUpdateMask |= ATTRS_MVA_UPDATED;

					if ( !iLen )
					{
						sphSetRowAttr ( const_cast<CSphRowitem *>( pRow ), dLocators[iCol], 0 );
						continue;
					}

					bool bDst64 = ( ( uDst64 & ( U64C(1) << iCol ) )!=0 );
					assert ( ( iLen%2 )==0 );
					DWORD uCount = ( bDst64 ? iLen : iLen/2 );

					CSphTightVector<DWORD> & dStorageMVA = pSegment->m_dMvas;
					DWORD uMvaOff = MVA_DOWNSIZE ( sphGetRowAttr ( pRow, dLocators[iCol] ) );
					assert ( uMvaOff<(DWORD)dStorageMVA.GetLength() );
					DWORD * pDst = dStorageMVA.Begin() + uMvaOff;
					if ( uCount>(*pDst) )
					{
						uMvaOff = dStorageMVA.GetLength();
						dStorageMVA.Resize ( uMvaOff+uCount+1 );
						pDst = dStorageMVA.Begin()+uMvaOff;
						sphSetRowAttr ( const_cast<CSphRowitem *>( pRow ), dLocators[iCol], uMvaOff );
					}

					if ( bDst64 )
					{
						memcpy ( pDst, pSrc, sizeof(DWORD)*(uCount+1) );
					} else
					{
						*pDst++ = uCount; // MVA values counter first
						pSrc++;
						while ( uCount-- )
						{
							*pDst = *pSrc;
							pDst++;
							pSrc+=2;
						}
					}
				}
			}

			if ( bUpdated )
				iUpdated++;

			break;
		}
		if ( bUpdated )
			continue;

		// check disk K-list now
		const SphDocID_t uRef = ( tUpd.m_dRows[iUpd] ? DOCINFO2ID ( tUpd.m_dRows[iUpd] ) : tUpd.m_dDocids[iUpd] );
		if ( dKilled.BinarySearch ( uRef )!=NULL )
			continue;

		// finally, try disk chunks
		for ( int iChunk = tGuard.m_dDiskChunks.GetLength()-1; iChunk>=0; iChunk-- )
		{
			// run just this update
			// FIXME! might be inefficient in case of big batches (redundant allocs in disk update)
			int iRes = const_cast<CSphIndex *>( tGuard.m_dDiskChunks[iChunk] )->UpdateAttributes ( tUpd, iUpd, sError, sWarning );

			// errors are highly unlikely at this point
			// FIXME! maybe emit a warning to client as well?
			if ( iRes<0 )
			{
				sphWarn ( "INTERNAL ERROR: index %s chunk %d update failure: %s", m_sIndexName.cstr(), iChunk, sError.cstr() );
				continue;
			}

			// update stats
			iUpdated += iRes;
			m_uDiskAttrStatus |= tGuard.m_dDiskChunks[iChunk]->GetAttributeStatus();

			// we only need to update the most fresh chunk
			if ( iRes>0 )
				break;
		}
	}

	// bump the counter, binlog the update!
	assert ( iIndex<0 );
	g_pBinlog->BinlogUpdateAttributes ( &m_iTID, m_sIndexName.cstr(), tUpd );

	if ( iJsonWarnings>0 )
	{
		sWarning.SetSprintf ( "%d attribute(s) can not be updated (not found or incompatible types)", iJsonWarnings );
		if ( iUpdated==0 )
		{
			sError = sWarning;
			return -1;
		}
	}

	// all done
	return iUpdated;
}


bool RtIndex_t::SaveAttributes ( CSphString & sError ) const
{
	if ( !m_dDiskChunks.GetLength() )
		return true;

	DWORD uStatus = m_uDiskAttrStatus;
	bool bAllSaved = true;

	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	ARRAY_FOREACH ( i, tGuard.m_dDiskChunks )
	{
		bAllSaved &= tGuard.m_dDiskChunks[i]->SaveAttributes ( sError );
	}

	if ( uStatus==m_uDiskAttrStatus )
		m_uDiskAttrStatus = 0;

	return bAllSaved;
}


struct SphOptimizeGuard_t : ISphNoncopyable
{
	CSphMutex &			m_tLock;
	volatile bool &		m_bOptimizeStop;

	SphOptimizeGuard_t ( CSphMutex & tLock, volatile bool & bOptimizeStop )
		: m_tLock ( tLock )
		, m_bOptimizeStop ( bOptimizeStop )
	{
		bOptimizeStop = true;
		m_tLock.Lock();
	}

	~SphOptimizeGuard_t ()
	{
		m_bOptimizeStop = false;
		m_tLock.Unlock();
	}
};


bool RtIndex_t::AddRemoveAttribute ( bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError )
{
	if ( m_dDiskChunks.GetLength() && !m_tSchema.GetAttrsCount() )
	{
		sError = "index must already have attributes";
		return false;
	}

	SphOptimizeGuard_t tStopOptimize ( m_tOptimizingLock, m_bOptimizeStop ); // got write-locked at daemon

	int iOldStride = m_iStride;
	int iOldRowSize = m_tSchema.GetRowSize();
	const CSphColumnInfo * pNewAttr = NULL;
	CSphSchema tOldSchema = m_tSchema;

	if ( bAdd )
	{
		CSphColumnInfo tInfo ( sAttrName.cstr(), eAttrType );
		m_tSchema.AddAttr ( tInfo, false );
		pNewAttr = m_tSchema.GetAttr ( sAttrName.cstr() );
	} else
		m_tSchema.RemoveAttr ( sAttrName.cstr(), false );

	m_iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();

	CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, false );

	// modify the in-memory data of disk chunks
	// fixme: we can't rollback in-memory changes, so we just show errors here for now
	ARRAY_FOREACH ( iDiskChunk, m_dDiskChunks )
		if ( !m_dDiskChunks[iDiskChunk]->AddRemoveAttribute ( bAdd, sAttrName, eAttrType, sError ) )
			sphWarning ( "%s attribute to %s.%d: %s", bAdd ? "adding" : "removing", m_sPath.cstr(), dChunkNames[iDiskChunk], sError.cstr() );

	// now modify the ramchunk
	ARRAY_FOREACH ( iSegment, m_dRamChunks )
	{
		RtSegment_t * pSeg = m_dRamChunks[iSegment];
		assert ( pSeg );
		CSphTightVector<CSphRowitem> dNewRows;
		dNewRows.Resize ( pSeg->m_dRows.GetLength() / iOldStride * m_iStride );
		CSphRowitem * pOldDocinfo = pSeg->m_dRows.Begin();
		CSphRowitem * pOldDocinfoEnd = pOldDocinfo+pSeg->m_dRows.GetLength();
		CSphRowitem * pNewDocinfo = dNewRows.Begin();

		if ( bAdd )
		{
			while ( pOldDocinfo < pOldDocinfoEnd )
			{
				SphDocID_t uDocId = DOCINFO2ID ( pOldDocinfo );
				DWORD * pAttrs = DOCINFO2ATTRS ( pOldDocinfo );
				memcpy ( DOCINFO2ATTRS ( pNewDocinfo ), pAttrs, iOldRowSize*sizeof(CSphRowitem) );
				sphSetRowAttr ( DOCINFO2ATTRS ( pNewDocinfo ), pNewAttr->m_tLocator, 0 );
				DOCINFOSETID ( pNewDocinfo, uDocId );
				pOldDocinfo += iOldStride;
				pNewDocinfo += m_iStride;
			}
		} else
		{
			int iAttrToRemove = tOldSchema.GetAttrIndex ( sAttrName.cstr() );

			CSphVector<int> dAttrMap;
			dAttrMap.Resize ( tOldSchema.GetAttrsCount() );
			for ( int iAttr = 0; iAttr < tOldSchema.GetAttrsCount(); iAttr++ )
				if ( iAttr!=iAttrToRemove )
				{
					dAttrMap[iAttr] = m_tSchema.GetAttrIndex ( tOldSchema.GetAttr ( iAttr ).m_sName.cstr() );
					assert ( dAttrMap[iAttr]>=0 );
				} else
					dAttrMap[iAttr] = -1;

			while ( pOldDocinfo < pOldDocinfoEnd )
			{
				DWORD * pOldAttrs = DOCINFO2ATTRS ( pOldDocinfo );
				DWORD * pNewAttrs = DOCINFO2ATTRS ( pNewDocinfo );

				for ( int iAttr = 0; iAttr < tOldSchema.GetAttrsCount(); iAttr++ )
					if ( iAttr!=iAttrToRemove )
					{
						SphAttr_t tValue = sphGetRowAttr ( pOldAttrs, tOldSchema.GetAttr ( iAttr ).m_tLocator );
						sphSetRowAttr ( pNewAttrs, m_tSchema.GetAttr ( dAttrMap[iAttr] ).m_tLocator, tValue );
					}

				DOCINFOSETID ( pNewDocinfo, DOCINFO2ID ( pOldDocinfo ) );

				pOldDocinfo += iOldStride;
				pNewDocinfo += m_iStride;
			}
		}

		pSeg->m_dRows.SwapData ( dNewRows );
	}

	// fixme: we can't rollback at this point
	Verify ( SaveRamChunk () );

	SaveMeta ( m_iTID, dChunkNames );

	// fixme: notify that it was ALTER that caused the flush
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	return true;
}

//////////////////////////////////////////////////////////////////////////
// MAGIC CONVERSIONS
//////////////////////////////////////////////////////////////////////////

bool RtIndex_t::AttachDiskIndex ( CSphIndex * pIndex, CSphString & sError )
{
	SphOptimizeGuard_t tStopOptimize ( m_tOptimizingLock, m_bOptimizeStop ); // got write-locked at daemon

	bool bEmptyRT = ( !m_dRamChunks.GetLength() && !m_dDiskChunks.GetLength() );

	// safeguards
	// we do not support some of the disk index features in RT just yet
#define LOC_ERROR(_arg) { sError = _arg; return false; }
	const CSphIndexSettings & tSettings = pIndex->GetSettings();
	if ( tSettings.m_iBoundaryStep!=0 )
		LOC_ERROR ( "ATTACH currently requires boundary_step=0 in disk index (RT-side support not implemented yet)" );
	if ( tSettings.m_iStopwordStep!=1 )
		LOC_ERROR ( "ATTACH currently requires stopword_step=1 in disk index (RT-side support not implemented yet)" );
	if ( tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN )
		LOC_ERROR ( "ATTACH currently requires docinfo=extern in disk index (RT-side support not implemented yet)" );
	// ATTACH to exist index require these checks
	if ( !bEmptyRT )
	{
		if ( m_pTokenizer->GetSettingsFNV()!=pIndex->GetTokenizer()->GetSettingsFNV() )
			LOC_ERROR ( "ATTACH currently requires same tokenizer settings (RT-side support not implemented yet)" );
		if ( m_pDict->GetSettingsFNV()!=pIndex->GetDictionary()->GetSettingsFNV() )
			LOC_ERROR ( "ATTACH currently requires same dictionary settings (RT-side support not implemented yet)" );
		if ( !GetMatchSchema().CompareTo ( pIndex->GetMatchSchema(), sError, true ) )
			LOC_ERROR ( "ATTACH currently requires same attributes declaration (RT-side support not implemented yet)" );
	}
#undef LOC_ERROR

	if ( !bEmptyRT )
	{
		SphAttr_t * pIndexDocList = nullptr;
		int64_t iCount = 0;
		if ( !pIndex->BuildDocList ( &pIndexDocList, &iCount, &sError ) )
		{
			sError.SetSprintf ( "ATTACH failed, %s", sError.cstr() );
			return false;
		}

		// new[] might fail on 32bit here
		// sphSort is 32bit too
		int64_t iSizeMax = (size_t)( iCount + pIndex->GetKillListSize() );
		if ( iCount + pIndex->GetKillListSize()!=iSizeMax )
		{
			SafeDeleteArray ( pIndexDocList );
			sError.SetSprintf ( "ATTACH failed, documents overflow (count=" INT64_FMT ", size max=" INT64_FMT ")", iCount + pIndex->GetKillListSize(), iSizeMax );
			return false;
		}

		CSphVector<SphDocID_t> dCombined;
		dCombined.Append ( ( SphDocID_t * ) pIndexDocList, iCount );
		dCombined.Append ( pIndex->GetKillList (), pIndex->GetKillListSize () );

		SafeDeleteArray ( pIndexDocList );

		m_dDiskChunkKlist.Resize ( 0 );
		m_tKlist.Flush ( m_dDiskChunkKlist );
		SphChunkGuard_t tGuard;
		GetReaderChunks ( tGuard );

		ChunkStats_t tStats ( m_tStats, m_dFieldLensRam );
		SaveDiskChunk ( m_iTID, tGuard, tStats, true );

		int64_t iKeep = 0;

		// kill-list drying up
		for ( int iIndex=m_dDiskChunks.GetLength()-1; iIndex>=0 && dCombined.GetLength (); --iIndex )
		{
			const CSphIndex * pDiskIndex = m_dDiskChunks[iIndex];
			for ( int64_t iID = iKeep; iID<dCombined.GetLength (); ++iID )
			{
				SphDocID_t uDocid = dCombined[iID];
				if ( !pDiskIndex->HasDocid ( uDocid ) )
				{
					// no duplicates - no need to keep ID in kill-list
					if ( iIndex==0 )
					{
						// RemoveFast
						dCombined.RemoveFast (iID);
					}
					continue;
				}

				// we just found the most recent chunk with our suspect docid
				// let's check whether it's already killed by subsequent chunks, or gets killed now
				bool bKeep = true;
				for ( int k=iIndex+1; k<m_dDiskChunks.GetLength() && bKeep; k++ )
				{
					const CSphIndex * pKilled = m_dDiskChunks[k];
					bKeep = ( sphBinarySearch ( pKilled->GetKillList(), pKilled->GetKillList() + pKilled->GetKillListSize() - 1, uDocid )==NULL );
				}

				if ( !bKeep )
				{
					// RemoveFast
					dCombined.RemoveFast ( iID );
				} else
				{
					Swap ( dCombined[iID], dCombined[iKeep] );
					++iKeep;
				}
			}
		}

		// sort by id and got rid of duplicates
		dCombined.Uniq();

		iSizeMax = (size_t)dCombined.GetLength64();
		if ( dCombined.GetLength ()!=iSizeMax )
		{
			sError.SetSprintf ( "ATTACH failed, kill-list overflow (size=" INT64_FMT ", size max=" INT64_FMT ")", dCombined.GetLength64 (), iSizeMax );
			return false;
		}

		bool bKillListDone = pIndex->ReplaceKillList ( dCombined.begin(), dCombined.GetLength() );

		if ( !bKillListDone )
		{
			sError.SetSprintf ( "ATTACH failed, kill-list replacement error (error='%s', warning='%s'", pIndex->GetLastError().cstr(), pIndex->GetLastWarning().cstr() );
			return false;
		}
	} // if ( !bEmptyRT )

	CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, true );

	// rename that source index to our last chunk
	CSphString sChunk;
	sChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), dChunkNames.Last() );
	if ( !pIndex->Rename ( sChunk.cstr() ) )
	{
		sError.SetSprintf ( "ATTACH failed, %s", pIndex->GetLastError().cstr() );
		return false;
	}

	// copy schema from new index
	m_tSchema = pIndex->GetMatchSchema();
	m_iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	m_tStats.m_iTotalBytes += pIndex->GetStats().m_iTotalBytes;
	m_tStats.m_iTotalDocuments += pIndex->GetStats().m_iTotalDocuments;

	// copy tokenizer, dict etc settings from new index
	m_tSettings = pIndex->GetSettings();
	m_tSettings.m_dBigramWords.Reset();
	m_tSettings.m_eDocinfo = SPH_DOCINFO_EXTERN;

	m_pTokenizer = pIndex->GetTokenizer()->Clone ( SPH_CLONE_INDEX );
	m_pDict = pIndex->GetDictionary()->Clone ();
	PostSetup();
	CSphString sName;
	sName.SetSprintf ( "%s_%d", m_sIndexName.cstr(), m_dDiskChunks.GetLength() );
	pIndex->SetName ( sName.cstr() );
	pIndex->SetBinlog ( false );

	// FIXME? what about copying m_TID etc?

	// recreate disk chunk list, resave header file
	m_dDiskChunks.Add ( pIndex );
	SaveMeta ( m_iTID, dChunkNames );

	// FIXME? do something about binlog too?
	// g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// all done, reset cache
	QcacheDeleteIndex ( GetIndexId() );
	return true;
}

//////////////////////////////////////////////////////////////////////////
// TRUNCATE
//////////////////////////////////////////////////////////////////////////

bool RtIndex_t::Truncate ( CSphString & )
{
	// TRUNCATE needs an exclusive lock, should be write-locked at daemon, conflicts only with optimize
	SphOptimizeGuard_t tStopOptimize ( m_tOptimizingLock, m_bOptimizeStop );

	// update and save meta
	// indicate 0 disk chunks, we are about to kill them anyway
	// current TID will be saved, so replay will properly skip preceding txns
	m_tStats.Reset();
	SaveMeta ( m_iTID, CSphFixedVector<int>(0) );

	// allow binlog to unlink now-redundant data files
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// kill RAM chunk file
	CSphString sFile;
	sFile.SetSprintf ( "%s.ram", m_sPath.cstr() );
	if ( ::unlink ( sFile.cstr() ) )
		if ( errno!=ENOENT )
			sphWarning ( "rt: truncate failed to unlink %s: %s", sFile.cstr(), strerrorm(errno) );

	// kill all disk chunks files
	ARRAY_FOREACH ( i, m_dDiskChunks )
	{
		StrVec_t v;
		const char * sChunkFilename = m_dDiskChunks[i]->GetFilename();
		sphSplit ( v, sChunkFilename, "." ); // split something like "rt.1"
		const char * sChunkNumber = v.Last().cstr();
		sFile.SetSprintf ( "%s.%s", m_sPath.cstr(), sChunkNumber );
		sphUnlinkIndex ( sFile.cstr(), false );
	}

	// kill in-memory data, reset stats
	ARRAY_FOREACH ( i, m_dDiskChunks )
		SafeDelete ( m_dDiskChunks[i] );
	m_dDiskChunks.Reset();

	ARRAY_FOREACH ( i, m_dRamChunks )
		SafeDelete ( m_dRamChunks[i] );
	m_dRamChunks.Reset();

	// we don't want kill list to work if we perform ATTACH right after this TRUNCATE
	m_tKlist.Reset ( NULL, 0 );

	// reset cache
	QcacheDeleteIndex ( GetIndexId() );
	return true;
}


//////////////////////////////////////////////////////////////////////////
// OPTIMIZE
//////////////////////////////////////////////////////////////////////////

void RtIndex_t::Optimize ( )
{
	if ( g_bProgressiveMerge )
	{
		ProgressiveMerge ( );
		return;
	}

	int64_t tmStart = sphMicroTimer();

	CSphScopedLock<CSphMutex> tOptimizing ( m_tOptimizingLock );
	m_bOptimizing = true;

	int iChunks = m_dDiskChunks.GetLength();
	CSphSchema tSchema = m_tSchema;
	CSphString sError;

	while ( m_dDiskChunks.GetLength()>1 && !g_bShutdown && !m_bOptimizeStop )
	{
		CSphVector<SphDocID_t> dKlist;

		// make kill-list
		// initially add RAM kill-list
		dKlist.Resize ( 0 );
		m_tKlist.Flush ( dKlist );

		const CSphIndex * pOldest = nullptr;
		const CSphIndex * pOlder = nullptr;
		{ // m_tChunkLock scoped Readlock
			CSphScopedRLock RChunkLock { m_tChunkLock };
			// merge 'older'(pSrc) to 'oldest'(pDst) and get 'merged' that names like 'oldest'+.tmp
			// to got rid of keeping actual kill-list
			// however 'merged' got placed at 'older' position and 'merged' renamed to 'older' name
			pOldest = m_dDiskChunks[0];
			pOlder = m_dDiskChunks[1];

			// add disk chunks kill-lists from next to oldest up to the newest, but not the oldest one
			for ( int iChunk=1; iChunk<m_dDiskChunks.GetLength(); iChunk++ )
			{
				if ( g_bShutdown || m_bOptimizeStop )
					break;

				const CSphIndex * pIndex = m_dDiskChunks[iChunk];
				dKlist.Append ( pIndex->GetKillList(), pIndex->GetKillListSize() );
			}
		} // m_tChunkLock scoped Readlock

		dKlist.Add ( 0 );
		dKlist.Add ( DOCID_MAX );
		dKlist.Uniq();

		CSphString sOlder, sOldest, sRename, sMerged;
		sOlder.SetSprintf ( "%s", pOlder->GetFilename() );
		sOldest.SetSprintf ( "%s", pOldest->GetFilename() );
		sRename.SetSprintf ( "%s.old", pOlder->GetFilename() );
		sMerged.SetSprintf ( "%s.tmp", pOldest->GetFilename() );

		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// merge data to disk ( data is constant during that phase )
		CSphIndexProgress tProgress;
		bool bMerged = sphMerge ( pOldest, pOlder, dKlist, sError, tProgress, &m_bOptimizeStop, true );
		if ( !bMerged )
		{
			sphWarning ( "rt optimize: index %s: failed to merge %s to %s (error %s)",
				m_sIndexName.cstr(), sOlder.cstr(), sOldest.cstr(), sError.cstr() );
			break;
		}
		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		CSphScopedPtr<CSphIndex> pMerged ( LoadDiskChunk ( sMerged.cstr(), sError ) );
		if ( !pMerged.Ptr() )
		{
			sphWarning ( "rt optimize: index %s: failed to load merged chunk (error %s)",
				m_sIndexName.cstr(), sError.cstr() );
			break;
		}
		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// lets rotate indexes

		// rename older disk chunk to 'old'
		if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sRename.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: cur to old rename failed (error %s)",
				m_sIndexName.cstr(), pOlder->GetLastError().cstr() );
			break;
		}
		// rename merged disk chunk to 0
		if ( !pMerged->Rename ( sOlder.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: merged to cur rename failed (error %s)",
				m_sIndexName.cstr(), pMerged->GetLastError().cstr() );
			if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sOlder.cstr() ) )
			{
				sphWarning ( "rt optimize: index %s: old to cur rename failed (error %s)",
					m_sIndexName.cstr(), pOlder->GetLastError().cstr() );
			}
			break;
		}

		if ( g_bShutdown || m_bOptimizeStop ) // protection
			break;

		Verify ( m_tWriting.Lock() );
		Verify ( m_tChunkLock.WriteLock() );

		sphLogDebug ( "optimized 0=%s, 1=%s, new=%s", m_dDiskChunks[0]->GetName(), m_dDiskChunks[1]->GetName(), pMerged->GetName() );

		m_dDiskChunks[1] = pMerged.LeakPtr();
		m_dDiskChunks.Remove ( 0 );
		CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, false );

		Verify ( m_tChunkLock.Unlock() );
		SaveMeta ( m_iTID, dChunkNames );
		Verify ( m_tWriting.Unlock() );

		if ( g_bShutdown || m_bOptimizeStop )
		{
			sphWarning ( "rt optimize: index %s: forced to shutdown, remove old index files manually '%s', '%s'",
				m_sIndexName.cstr(), sRename.cstr(), sOldest.cstr() );
			break;
		}

		// exclusive reader (to make sure that disk chunks not used any more) and writer lock here
		// write lock goes first as with commit
		Verify ( m_tWriting.Lock() );
		Verify ( m_tReading.WriteLock() );

		SafeDelete ( pOlder );
		SafeDelete ( pOldest );

		Verify ( m_tReading.Unlock() );
		Verify ( m_tWriting.Unlock() );

		// we might remove old index files
		sphUnlinkIndex ( sRename.cstr(), true );
		sphUnlinkIndex ( sOldest.cstr(), true );
		// FIXEME: wipe out 'merged' index files in case of error
	}

	m_bOptimizing = false;
	int64_t tmPass = sphMicroTimer() - tmStart;

	if ( g_bShutdown )
	{
		sphWarning ( "rt: index %s: optimization terminated chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	} else
	{
		sphInfo ( "rt: index %s: optimized chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	}
}


//////////////////////////////////////////////////////////////////////////
// PROGRESSIVE MERGE
//////////////////////////////////////////////////////////////////////////

static int64_t GetChunkSize ( const CSphVector<CSphIndex*> & dDiskChunks, int iIndex )
{
	if (iIndex<0)
		return 0;
	CSphIndexStatus tDisk;
	dDiskChunks[iIndex]->GetStatus(&tDisk);
	return tDisk.m_iDiskUse;
}


static int GetNextSmallestChunk ( const CSphVector<CSphIndex*> & dDiskChunks, int iIndex )
{
	assert (dDiskChunks.GetLength ()>1);
	int iRes = -1;
	int64_t iLastSize = INT64_MAX;
	ARRAY_FOREACH ( i, dDiskChunks )
	{
		int64_t iSize = GetChunkSize ( dDiskChunks, i );
		if ( iSize<iLastSize && iIndex!=i )
		{
			iLastSize = iSize;
			iRes = i;
		}
	}
	return iRes;
}


void RtIndex_t::ProgressiveMerge ()
{
	// How does this work:
	// In order to minimize IO operations we merge chunks in order from the smallest to the largest to build a progression
	// Applying kill-lists is where it all gets complicated (kill-lists must take the chronology into account)
	// 1) On every step, select two smallest chunks, A and B (A also should be older than B).
	// 2) collect all kill-lists from A to B (inclusive)
	// 3) merge A and A+1 kill-lists, write them to A+1
	// 4) merge A and B chunk data to A, apply all kill lists collected on step 2
	// the timeline is: [older chunks], ..., A, A+1, ..., B, ..., [younger chunks]
	// this also needs meta v.12 (chunk list with possible skips, instead of a base chunk + length as in meta v.11)

	int64_t tmStart = sphMicroTimer();

	CSphScopedLock<CSphMutex> tOptimizing ( m_tOptimizingLock );
	m_bOptimizing = true;

	int iChunks = m_dDiskChunks.GetLength();
	CSphSchema tSchema = m_tSchema;
	CSphString sError;

	while ( m_dDiskChunks.GetLength()>1 && !g_bShutdown && !m_bOptimizeStop )
	{
		CSphVector<SphDocID_t> dKlist;
		CSphVector<SphDocID_t> dMergedKlist;

		// make kill-list
		// initially add RAM kill-list
		dKlist.Resize ( 0 );
		m_tKlist.Flush ( dKlist );

		const CSphIndex * pOldest = nullptr;
		const CSphIndex * pOlder = nullptr;
		int iA = -1;
		int iB = -1;

		{
			CSphScopedRLock tChunkLock { m_tChunkLock };

			// merge 'smallest' to 'smaller' and get 'merged' that names like 'A'+.tmp
			// however 'merged' got placed at 'B' position and 'merged' renamed to 'B' name

			iA = GetNextSmallestChunk ( m_dDiskChunks, 0 );
			iB = GetNextSmallestChunk ( m_dDiskChunks, iA );

			if ( iA<0 || iB<0 )
			{
				sError.SetSprintf ("Couldn't find smallest chunk");
				return;
			}

			// in order to merge kill-lists correctly we need to make sure that A is the oldest one
			// indexes go from oldest to newest so A must go before B (A is always older than B)
			if ( iA > iB )
				Swap ( iB, iA );

			sphLogDebug ( "progressive merge - merging %d (%d kb) with %d (%d kb)", iA, (int)(GetChunkSize ( m_dDiskChunks, iA )/1024), iB, (int)(GetChunkSize ( m_dDiskChunks, iB )/1024) );

			pOldest = m_dDiskChunks[iA];
			pOlder = m_dDiskChunks[iB];

			// so we're merging chunk A (older) with the (younger) B, so A < B
			// we have to merge chunk's A kill-list to chunk A+1 to maintain the integrity

			// collect all kill-lists from A to newest (inclusive), but not A itself
			for ( int iChunk=iA+1; iChunk<=m_dDiskChunks.GetLength()-1; iChunk++ )
			{
				if ( g_bShutdown || m_bOptimizeStop )
					break;

				const CSphIndex * pIndex = m_dDiskChunks[iChunk];
				if ( !pIndex->GetKillListSize() )
					continue;

				dKlist.Append ( pIndex->GetKillList (), pIndex->GetKillListSize ());
			}

			// merge klist from oldest disk chunk to next disk chunk
			// that might be either A and A+1 or A and B
			// but not set kill-list for oldest disk chunk as it useless and only consumes memory
			if ( iA!=0 )
			{
				CSphIndex * pNextChunk = m_dDiskChunks[iA + 1];
				dMergedKlist.Append ( pOldest->GetKillList (), pOldest->GetKillListSize () );
				dMergedKlist.Append ( pNextChunk->GetKillList (), pNextChunk->GetKillListSize () );
			}
		} // m_tChunkLock scope

		// for filtering have to set bounds
		dKlist.Add ( 0 );
		dKlist.Add ( DOCID_MAX );
		dKlist.Uniq();
		// got rid of duplicates at A+1 klist
		dMergedKlist.Uniq();

		CSphString sOlder, sOldest, sRename, sMerged;
		sOlder.SetSprintf ( "%s", pOlder->GetFilename() );
		sOldest.SetSprintf ( "%s", pOldest->GetFilename() );
		sRename.SetSprintf ( "%s.old", pOlder->GetFilename() );
		sMerged.SetSprintf ( "%s.tmp", pOldest->GetFilename() );

		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// merge data to disk ( data is constant during that phase )
		CSphIndexProgress tProgress;
		bool bMerged = sphMerge ( pOldest, pOlder, dKlist, sError, tProgress, &m_bOptimizeStop, true );
		if ( !bMerged )
		{
			sphWarning ( "rt optimize: index %s: failed to merge %s to %s (error %s)",
				m_sIndexName.cstr(), sOlder.cstr(), sOldest.cstr(), sError.cstr() );
			break;
		}
		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		CSphScopedPtr<CSphIndex> pMerged ( LoadDiskChunk ( sMerged.cstr(), sError ) );
		if ( !pMerged.Ptr() )
		{
			sphWarning ( "rt optimize: index %s: failed to load merged chunk (error %s)",
				m_sIndexName.cstr(), sError.cstr() );
			break;
		}
		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// lets rotate indexes

		// rename older disk chunk to 'old'
		if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sRename.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: cur to old rename failed (error %s)",
				m_sIndexName.cstr(), pOlder->GetLastError().cstr() );
			break;
		}
		// rename merged disk chunk to B
		if ( !pMerged->Rename ( sOlder.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: merged to cur rename failed (error %s)",
				m_sIndexName.cstr(), pMerged->GetLastError().cstr() );
			if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sOlder.cstr() ) )
			{
				sphWarning ( "rt optimize: index %s: old to cur rename failed (error %s)",
					m_sIndexName.cstr(), pOlder->GetLastError().cstr() );
			}
			break;
		}

		if ( g_bShutdown || m_bOptimizeStop ) // protection
			break;

		// merged replaces recent chunk
		// oldest chunk got deleted
		// next after oldest keeps klist from oldest

		// Writing lock - to wipe out writers
		// Reading wlock - to wipe out searches as we replacing klist
		// Chunk wlock - to lock chunks as going to modify chinks vector
		// order same as GetReaderChunks and SaveDiskChunk to prevent deadlock

		Verify ( m_tWriting.Lock() );
		Verify ( m_tReading.WriteLock() );
		Verify ( m_tChunkLock.WriteLock() );

		sphLogDebug ( "optimized (progressive) a=%s, b=%s, new=%s", pOldest->GetName(), pOlder->GetName(), pMerged->GetName() );

		m_dDiskChunks[iB] = pMerged.LeakPtr();
		// move merged klist to next after oldest disk chunk
		m_dDiskChunks[iA+1]->ReplaceKillList ( dMergedKlist.Begin(), dMergedKlist.GetLength() );
		m_dDiskChunks.Remove ( iA );
		CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, false );

		Verify ( m_tChunkLock.Unlock() );
		Verify ( m_tReading.Unlock() );
		SaveMeta ( m_iTID, dChunkNames );
		Verify ( m_tWriting.Unlock() );

		if ( g_bShutdown || m_bOptimizeStop )
		{
			sphWarning ( "rt optimize: index %s: forced to shutdown, remove old index files manually '%s', '%s'",
				m_sIndexName.cstr(), sRename.cstr(), sOldest.cstr() );
			break;
		}

		// exclusive reader (to make sure that disk chunks not used any more) and writer lock here
		// wipe out writer then way all readers get out - to delete indexes
		// as readers might keep copy of chunks vector
		Verify ( m_tWriting.Lock() );
		Verify ( m_tReading.WriteLock() );

		SafeDelete ( pOlder );
		SafeDelete ( pOldest );

		Verify ( m_tReading.Unlock() );
		Verify ( m_tWriting.Unlock() );

		// we might remove old index files
		sphUnlinkIndex ( sRename.cstr(), true );
		sphUnlinkIndex ( sOldest.cstr(), true );
		// FIXEME: wipe out 'merged' index files in case of error
	}

	m_bOptimizing = false;
	int64_t tmPass = sphMicroTimer() - tmStart;

	if ( g_bShutdown )
	{
		sphWarning ( "rt: index %s: optimization terminated chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	} else
	{
		sphInfo ( "rt: index %s: optimized (progressive) chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	}
}


//////////////////////////////////////////////////////////////////////////
// STATUS
//////////////////////////////////////////////////////////////////////////

void RtIndex_t::GetStatus ( CSphIndexStatus * pRes ) const
{
	assert ( pRes );
	if ( !pRes )
		return;

	Verify ( m_tChunkLock.ReadLock() );

	pRes->m_iRamChunkSize = GetUsedRam()
		+ m_dRamChunks.AllocatedBytes ()
		+ m_dRamChunks.GetLength()*int(sizeof(RtSegment_t))
		+ m_dNewSegmentKlist.AllocatedBytes ();

	pRes->m_iRamUse = sizeof(RtIndex_t)
		+ m_dDiskChunkKlist.AllocatedBytes ()
		+ m_dDiskChunks.AllocatedBytes ()
		+ pRes->m_iRamChunkSize;

	pRes->m_iRamRetired = 0;
	ARRAY_FOREACH ( i, m_dRetired )
		pRes->m_iRamRetired += m_dRetired[i]->GetUsedRam();

	pRes->m_iMemLimit = m_iSoftRamLimit;
	pRes->m_iDiskUse = 0;

	CSphString sError;
	char sFile [ SPH_MAX_FILENAME_LEN ];
	const char * sFiles[] = { ".meta", ".kill", ".ram" };
	for ( const char * sName : sFiles )
	{
		snprintf ( sFile, sizeof(sFile), "%s%s", m_sFilename.cstr(), sName );
		CSphAutofile fdRT ( sFile, SPH_O_READ, sError );
		int64_t iFileSize = fdRT.GetSize();
		if ( iFileSize>0 )
			pRes->m_iDiskUse += iFileSize;
	}
	CSphIndexStatus tDisk;
	ARRAY_FOREACH ( i, m_dDiskChunks )
	{
		m_dDiskChunks[i]->GetStatus(&tDisk);
		pRes->m_iRamUse += tDisk.m_iRamUse;
		pRes->m_iDiskUse += tDisk.m_iDiskUse;
	}

	pRes->m_iNumChunks = m_dDiskChunks.GetLength();

	Verify ( m_tChunkLock.Unlock() );
}

//////////////////////////////////////////////////////////////////////////
// RECONFIGURE
//////////////////////////////////////////////////////////////////////////

bool CreateReconfigure ( const CSphString & sIndexName, bool bIsStarDict, const ISphFieldFilter * pFieldFilter,
	const CSphIndexSettings & tIndexSettings, uint64_t uTokHash, uint64_t uDictHash, int iMaxCodepointLength,
	bool bSame, CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError )
{
	// FIXME!!! check missed embedded files
	ISphTokenizerRefPtr_c pTokenizer { ISphTokenizer::Create ( tSettings.m_tTokenizer, NULL, sError ) };
	if ( !pTokenizer )
	{
		sError.SetSprintf ( "'%s' failed to create tokenizer, error '%s'", sIndexName.cstr(), sError.cstr() );
		return true;
	}

	// dict setup second
	CSphDictRefPtr_c tDict { sphCreateDictionaryCRC ( tSettings.m_tDict, NULL, pTokenizer, sIndexName.cstr(), sError ) };
	if ( !tDict )
	{
		sError.SetSprintf ( "'%s' failed to create dictionary, error '%s'", sIndexName.cstr(), sError.cstr() );
		return true;
	}

	// multiforms right after dict
	pTokenizer = ISphTokenizer::CreateMultiformFilter ( pTokenizer, tDict->GetMultiWordforms() );

	// bigram filter
	if ( tSettings.m_tIndex.m_eBigramIndex!=SPH_BIGRAM_NONE && tSettings.m_tIndex.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		pTokenizer->SetBuffer ( (BYTE*)tSettings.m_tIndex.m_sBigramWords.cstr(), tSettings.m_tIndex.m_sBigramWords.Length() );

		BYTE * pTok = NULL;
		while ( ( pTok = pTokenizer->GetToken() )!=NULL )
			tSettings.m_tIndex.m_dBigramWords.Add() = (const char*)pTok;

		tSettings.m_tIndex.m_dBigramWords.Sort();
	}

	bool bNeedExact = ( tDict->HasMorphology() || tDict->GetWordformsFileInfos().GetLength() );
	if ( tSettings.m_tIndex.m_bIndexExactWords && !bNeedExact )
		tSettings.m_tIndex.m_bIndexExactWords = false;

	if ( tDict->GetSettings().m_bWordDict && tDict->HasMorphology() && bIsStarDict && !tSettings.m_tIndex.m_bIndexExactWords )
		tSettings.m_tIndex.m_bIndexExactWords = true;

	// field filter
	ISphFieldFilterRefPtr_c tFieldFilter;

	// re filter
	bool bReFilterSame = true;
	CSphFieldFilterSettings tFieldFilterSettings;
	if ( pFieldFilter )
		pFieldFilter->GetSettings ( tFieldFilterSettings );
	if ( tFieldFilterSettings.m_dRegexps.GetLength()!=tSettings.m_tFieldFilter.m_dRegexps.GetLength() )
	{
		bReFilterSame = false;
	} else
	{
		CSphVector<uint64_t> dFieldFilter;
		ARRAY_FOREACH ( i, tFieldFilterSettings.m_dRegexps )
			dFieldFilter.Add ( sphFNV64 ( tFieldFilterSettings.m_dRegexps[i].cstr() ) );
		dFieldFilter.Uniq();
		uint64_t uMyFF = sphFNV64 ( dFieldFilter.Begin(), sizeof(dFieldFilter[0]) * dFieldFilter.GetLength() );

		dFieldFilter.Resize ( 0 );
		ARRAY_FOREACH ( i, tSettings.m_tFieldFilter.m_dRegexps )
			dFieldFilter.Add ( sphFNV64 ( tSettings.m_tFieldFilter.m_dRegexps[i].cstr() ) );
		dFieldFilter.Uniq();
		uint64_t uNewFF = sphFNV64 ( dFieldFilter.Begin(), sizeof(dFieldFilter[0]) * dFieldFilter.GetLength() );

		bReFilterSame = ( uMyFF==uNewFF );
	}

	if ( !bReFilterSame && tSettings.m_tFieldFilter.m_dRegexps.GetLength () )
	{
		tFieldFilter = sphCreateRegexpFilter ( tSettings.m_tFieldFilter, sError );
		if ( !tFieldFilter )
		{
			sError.SetSprintf ( "'%s' failed to create field filter, error '%s'", sIndexName.cstr (), sError.cstr () );
			return true;
		}
	}

	// rlp filter
	bool bRlpSame = ( tIndexSettings.m_eChineseRLP==tSettings.m_tIndex.m_eChineseRLP );
	if ( !bRlpSame )
	{
		if ( !sphSpawnRLPFilter ( tFieldFilter, tSettings.m_tIndex, tSettings.m_tTokenizer, sIndexName.cstr (), sError ) )
		{
			sError.SetSprintf ( "'%s' failed to create field filter, error '%s'", sIndexName.cstr (), sError.cstr () );
			return true;
		}
	}

	// compare options
	if ( !bSame || uTokHash!=pTokenizer->GetSettingsFNV() || uDictHash!=tDict->GetSettingsFNV() ||
		iMaxCodepointLength!=pTokenizer->GetMaxCodepointLength() || sphGetSettingsFNV ( tIndexSettings )!=sphGetSettingsFNV ( tSettings.m_tIndex ) ||
		!bReFilterSame || !bRlpSame )
	{
		tSetup.m_pTokenizer = pTokenizer.Leak();
		tSetup.m_pDict = tDict.Leak();
		tSetup.m_tIndex = tSettings.m_tIndex;
		tSetup.m_pFieldFilter = tFieldFilter.Leak();
		return false;
	} else
	{
		return true;
	}
}

bool RtIndex_t::IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError ) const
{
	return CreateReconfigure ( m_sIndexName, IsStarDict(), m_pFieldFilter, m_tSettings,
		m_pTokenizer->GetSettingsFNV(), m_pDict->GetSettingsFNV(), m_pTokenizer->GetMaxCodepointLength(), true, tSettings, tSetup, sError );
}

void RtIndex_t::Reconfigure ( CSphReconfigureSetup & tSetup )
{
	ForceDiskChunk();

	Setup ( tSetup.m_tIndex );
	SetTokenizer ( tSetup.m_pTokenizer );
	SetDictionary ( tSetup.m_pDict );
	SetFieldFilter ( tSetup.m_pFieldFilter );

	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();
	SetupQueryTokenizer();

	// FIXME!!! handle error
	m_pTokenizerIndexing = m_pTokenizer->Clone ( SPH_CLONE_INDEX );
	ISphTokenizerRefPtr_c pIndexing { ISphTokenizer::CreateBigramFilter ( m_pTokenizerIndexing, m_tSettings.m_eBigramIndex, m_tSettings.m_sBigramWords, m_sLastError ) };
	if ( pIndexing )
		m_pTokenizerIndexing = pIndexing;
}

uint64_t sphGetSettingsFNV ( const CSphIndexSettings & tSettings )
{
	uint64_t uHash = 0;

	DWORD uFlags = 0;
	if ( tSettings.m_bHtmlStrip )
		uFlags |= 1<<1;
	if ( tSettings.m_bIndexExactWords )
		uFlags |= 1<<2;
	if ( tSettings.m_bIndexFieldLens )
		uFlags |= 1<<3;
	if ( tSettings.m_bIndexSP )
		uFlags |= 1<<4;
	uHash = sphFNV64 ( &uFlags, sizeof(uFlags), uHash );

	uHash = sphFNV64 ( &tSettings.m_eHitFormat, sizeof(tSettings.m_eHitFormat), uHash );
	uHash = sphFNV64 ( tSettings.m_sHtmlIndexAttrs.cstr(), tSettings.m_sHtmlIndexAttrs.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sHtmlRemoveElements.cstr(), tSettings.m_sHtmlRemoveElements.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sZones.cstr(), tSettings.m_sZones.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_eHitless, sizeof(tSettings.m_eHitless), uHash );
	uHash = sphFNV64 ( tSettings.m_sHitlessFiles.cstr(), tSettings.m_sHitlessFiles.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_eBigramIndex, sizeof(tSettings.m_eBigramIndex), uHash );
	uHash = sphFNV64 ( tSettings.m_sBigramWords.cstr(), tSettings.m_sBigramWords.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_uAotFilterMask, sizeof(tSettings.m_uAotFilterMask), uHash );
	uHash = sphFNV64 ( &tSettings.m_eChineseRLP, sizeof(tSettings.m_eChineseRLP), uHash );
	uHash = sphFNV64 ( tSettings.m_sRLPContext.cstr(), tSettings.m_sRLPContext.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sIndexTokenFilter.cstr(), tSettings.m_sIndexTokenFilter.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMinPrefixLen, sizeof(tSettings.m_iMinPrefixLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMinInfixLen, sizeof(tSettings.m_iMinInfixLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMaxSubstringLen, sizeof(tSettings.m_iMaxSubstringLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iBoundaryStep, sizeof(tSettings.m_iBoundaryStep), uHash );
	uHash = sphFNV64 ( &tSettings.m_iOvershortStep, sizeof(tSettings.m_iOvershortStep), uHash );
	uHash = sphFNV64 ( &tSettings.m_iStopwordStep, sizeof(tSettings.m_iStopwordStep), uHash );

	return uHash;
}

//////////////////////////////////////////////////////////////////////////
// BINLOG
//////////////////////////////////////////////////////////////////////////

extern DWORD g_dSphinxCRC32 [ 256 ];


static CSphString MakeBinlogName ( const char * sPath, int iExt )
{
	CSphString sName;
	sName.SetSprintf ( "%s/binlog.%03d", sPath, iExt );
	return sName;
}


BinlogWriter_c::BinlogWriter_c ()
{
	m_iLastWritePos = 0;
	m_iLastFsyncPos = 0;
	m_iLastCrcPos = 0;
	ResetCrc();
}


void BinlogWriter_c::ResetCrc ()
{
	m_uCRC = ~((DWORD)0);
	m_iLastCrcPos = m_iPoolUsed;
}


void BinlogWriter_c::HashCollected ()
{
	assert ( m_iLastCrcPos<=m_iPoolUsed );

	const BYTE * b = m_pBuffer + m_iLastCrcPos;
	int iSize = m_iPoolUsed - m_iLastCrcPos;
	DWORD uCRC = m_uCRC;

	for ( int i=0; i<iSize; i++ )
		uCRC = (uCRC >> 8) ^ g_dSphinxCRC32 [ (uCRC ^ *b++) & 0xff ];

	m_iLastCrcPos = m_iPoolUsed;
	m_uCRC = uCRC;
}


void BinlogWriter_c::WriteCrc ()
{
	HashCollected();
	m_uCRC = ~m_uCRC;
	CSphWriter::PutDword ( m_uCRC );
	ResetCrc();
}


void BinlogWriter_c::Flush ()
{
	Write();
	Fsync();
	m_iLastCrcPos = m_iPoolUsed;
}


void BinlogWriter_c::Write ()
{
	if ( m_iPoolUsed<=0 )
		return;

	HashCollected();
	CSphWriter::Flush();
	m_iLastWritePos = GetPos();
}


#if USE_WINDOWS
int fsync ( int iFD )
{
	// map fd to handle
	HANDLE h = (HANDLE) _get_osfhandle ( iFD );
	if ( h==INVALID_HANDLE_VALUE )
	{
		errno = EBADF;
		return -1;
	}

	// do flush
	if ( FlushFileBuffers(h) )
		return 0;

	// error handling
	errno = EIO;
	if ( GetLastError()==ERROR_INVALID_HANDLE )
		errno = EINVAL;
	return -1;
}
#endif


void BinlogWriter_c::Fsync ()
{
	if ( !HasUnsyncedData() )
		return;

	m_bError = ( fsync ( m_iFD )!=0 );
	if ( m_bError && m_pError )
		m_pError->SetSprintf ( "failed to sync %s: %s" , m_sName.cstr(), strerrorm(errno) );

	m_iLastFsyncPos = GetPos();
}

//////////////////////////////////////////////////////////////////////////

BinlogReader_c::BinlogReader_c()
{
	ResetCrc ();
}

void BinlogReader_c::ResetCrc ()
{
	m_uCRC = ~(DWORD(0));
	m_iLastCrcPos = m_iBuffPos;
}


bool BinlogReader_c::CheckCrc ( const char * sOp, const char * sIndexName, int64_t iTid, int64_t iTxnPos )
{
	HashCollected ();
	DWORD uCRC = ~m_uCRC;
	DWORD uRef = CSphAutoreader::GetDword();
	ResetCrc();
	bool bPassed = ( uRef==uCRC );
	if ( !bPassed )
		sphWarning ( "binlog: %s: CRC mismatch (index=%s, tid=" INT64_FMT ", pos=" INT64_FMT ")", sOp, sIndexName ? sIndexName : "", iTid, iTxnPos );
	return bPassed;
}


void BinlogReader_c::UpdateCache ()
{
	HashCollected();
	CSphAutoreader::UpdateCache();
	m_iLastCrcPos = m_iBuffPos;
}

void BinlogReader_c::HashCollected ()
{
	assert ( m_iLastCrcPos<=m_iBuffPos );

	const BYTE * b = m_pBuff + m_iLastCrcPos;
	int iSize = m_iBuffPos - m_iLastCrcPos;
	DWORD uCRC = m_uCRC;

	for ( int i=0; i<iSize; i++ )
		uCRC = (uCRC >> 8) ^ g_dSphinxCRC32 [ (uCRC ^ *b++) & 0xff ];

	m_iLastCrcPos = m_iBuffPos;
	m_uCRC = uCRC;
}

//////////////////////////////////////////////////////////////////////////

RtBinlog_c::RtBinlog_c ()
	: m_iFlushTimeLeft ( 0 )
	, m_iFlushPeriod ( BINLOG_AUTO_FLUSH )
	, m_eOnCommit ( ACTION_NONE )
	, m_iLockFD ( -1 )
	, m_bReplayMode ( false )
	, m_bDisabled ( true )
	, m_iRestartSize ( 268435456 )
{
	MEMORY ( MEM_BINLOG );

	m_tWriter.SetBufferSize ( BINLOG_WRITE_BUFFER );
}

RtBinlog_c::~RtBinlog_c ()
{
	if ( !m_bDisabled )
	{
		m_iFlushPeriod = 0;
		DoCacheWrite();
		m_tWriter.CloseFile();
		LockFile ( false );
	}
}


void RtBinlog_c::BinlogCommit ( int64_t * pTID, const char * sIndexName, const RtSegment_t * pSeg,
	const CSphVector<SphDocID_t> & dKlist, bool bKeywordDict )
{
	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	Verify ( m_tWriteLock.Lock() );

	int64_t iTID = ++(*pTID);
	const int64_t tmNow = sphMicroTimer();
	const int uIndex = GetWriteIndexID ( sIndexName, iTID, tmNow );

	// header
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_COMMIT );
	m_tWriter.ZipOffset ( uIndex );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );

	// save txn data
	if ( !pSeg || !pSeg->m_iRows )
	{
		m_tWriter.ZipOffset ( 0 );
	} else
	{
		m_tWriter.ZipOffset ( pSeg->m_iRows );
		SaveVector ( m_tWriter, pSeg->m_dWords );
		m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints.GetLength() );
		if ( !bKeywordDict )
		{
			ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
			{
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_iOffset );
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_uWordID );
			}
		} else
		{
			const char * pBase = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
			ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
			{
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_iOffset );
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_sWord - pBase );
			}
		}
		SaveVector ( m_tWriter, pSeg->m_dDocs );
		SaveVector ( m_tWriter, pSeg->m_dHits );
		SaveVector ( m_tWriter, pSeg->m_dRows );
		SaveVector ( m_tWriter, pSeg->m_dStrings );
		SaveVector ( m_tWriter, pSeg->m_dMvas );
		SaveVector ( m_tWriter, pSeg->m_dKeywordCheckpoints );
	}
	SaveVector ( m_tWriter, dKlist );

	// checksum
	m_tWriter.WriteCrc ();

	// finalize
	CheckDoFlush();
	CheckDoRestart();
	Verify ( m_tWriteLock.Unlock() );
}

void RtBinlog_c::BinlogUpdateAttributes ( int64_t * pTID, const char * sIndexName, const CSphAttrUpdate & tUpd )
{
	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	Verify ( m_tWriteLock.Lock() );

	int64_t iTID = ++(*pTID);
	const int64_t tmNow = sphMicroTimer();
	const int uIndex = GetWriteIndexID ( sIndexName, iTID, tmNow );

	// header
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_UPDATE_ATTRS );
	m_tWriter.ZipOffset ( uIndex );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );

	// update data
	m_tWriter.ZipOffset ( tUpd.m_dAttrs.GetLength() );
	ARRAY_FOREACH ( i, tUpd.m_dAttrs )
	{
		m_tWriter.PutString ( tUpd.m_dAttrs[i] );
		m_tWriter.ZipOffset ( tUpd.m_dTypes[i] );
	}

	CSphVector<SphDocID_t> dActiveDocids;
	bool bUseRaw = false;
	if ( tUpd.m_dDocids.GetLength()==0 && tUpd.m_dRows.GetLength()!=0 )
	{
		bUseRaw = true;
		dActiveDocids.Resize ( tUpd.m_dRows.GetLength() );
		ARRAY_FOREACH ( i, tUpd.m_dRows )
			dActiveDocids[i] = DOCINFO2ID ( tUpd.m_dRows[i] );
	}
	const CSphVector<SphDocID_t> & dBinlogDocids = bUseRaw ? dActiveDocids : tUpd.m_dDocids;

	// POD vectors
	SaveVector ( m_tWriter, tUpd.m_dPool );
	SaveVector ( m_tWriter, dBinlogDocids );
	dActiveDocids.Reset();
	SaveVector ( m_tWriter, tUpd.m_dRowOffset );

	// checksum
	m_tWriter.WriteCrc ();

	// finalize
	CheckDoFlush();
	CheckDoRestart();
	Verify ( m_tWriteLock.Unlock() );
}

void RtBinlog_c::BinlogReconfigure ( int64_t * pTID, const char * sIndexName, const CSphReconfigureSetup & tSetup )
{
	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	Verify ( m_tWriteLock.Lock() );

	int64_t iTID = ++(*pTID);
	const int64_t tmNow = sphMicroTimer();
	const int uIndex = GetWriteIndexID ( sIndexName, iTID, tmNow );

	// header
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_RECONFIGURE );
	m_tWriter.ZipOffset ( uIndex );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );

	// reconfigure data
	SaveIndexSettings ( m_tWriter, tSetup.m_tIndex );
	SaveTokenizerSettings ( m_tWriter, tSetup.m_pTokenizer, 0 );
	SaveDictionarySettings ( m_tWriter, tSetup.m_pDict, false, 0 );
	SaveFieldFilterSettings ( m_tWriter, tSetup.m_pFieldFilter );

	// checksum
	m_tWriter.WriteCrc ();

	// finalize
	CheckDoFlush();
	CheckDoRestart();
	Verify ( m_tWriteLock.Unlock() );
}


// here's been going binlogs with ALL closed indices removing
void RtBinlog_c::NotifyIndexFlush ( const char * sIndexName, int64_t iTID, bool bShutdown )
{
	if ( m_bReplayMode )
		sphInfo ( "index '%s': ramchunk saved. TID=" INT64_FMT "", sIndexName, iTID );

	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	assert ( bShutdown || m_dLogFiles.GetLength() );

	Verify ( m_tWriteLock.Lock() );

	bool bCurrentLogShut = false;
	const int iPreflushFiles = m_dLogFiles.GetLength();

	// loop through all log files, and check if we can unlink any
	ARRAY_FOREACH ( iLog, m_dLogFiles )
	{
		BinlogFileDesc_t & tLog = m_dLogFiles[iLog];
		bool bUsed = false;

		// update index info for this log file
		ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
		{
			BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];

			// this index was just flushed, update flushed TID
			if ( tIndex.m_sName==sIndexName )
			{
				assert ( iTID>=tIndex.m_iFlushedTID );
				tIndex.m_iFlushedTID = Max ( tIndex.m_iFlushedTID, iTID );
			}

			// if max logged TID is greater than last flushed TID, log file still has needed recovery data
			if ( tIndex.m_iFlushedTID < tIndex.m_iMaxTID )
				bUsed = true;
		}

		// it's needed, keep looking
		if ( bUsed )
			continue;

		// hooray, we can remove this log!
		// if this is our current log, we have to close it first
		if ( iLog==m_dLogFiles.GetLength()-1 )
		{
			m_tWriter.CloseFile ();
			bCurrentLogShut = true;
		}

		// do unlink
		CSphString sLog = MakeBinlogName ( m_sLogPath.cstr(), tLog.m_iExt );
		if ( ::unlink ( sLog.cstr() ) )
			sphWarning ( "binlog: failed to unlink %s: %s (remove it manually)", sLog.cstr(), strerrorm(errno) );

		// we need to reset it, otherwise there might be leftover data after last Remove()
		m_dLogFiles[iLog] = BinlogFileDesc_t();
		// quit tracking it
		m_dLogFiles.Remove ( iLog-- );
	}

	if ( bCurrentLogShut && !bShutdown )
	{
		// if current log was closed, we need a new one (it will automatically save meta, too)
		OpenNewLog ();

	} else if ( iPreflushFiles!=m_dLogFiles.GetLength() )
	{
		// if we unlinked any logs, we need to save meta, too
		SaveMeta ();
	}

	Verify ( m_tWriteLock.Unlock() );
}

void RtBinlog_c::Configure ( const CSphConfigSection & hSearchd, bool bTestMode )
{
	MEMORY ( MEM_BINLOG );

	const int iMode = hSearchd.GetInt ( "binlog_flush", 2 );
	switch ( iMode )
	{
		case 0:		m_eOnCommit = ACTION_NONE; break;
		case 1:		m_eOnCommit = ACTION_FSYNC; break;
		case 2:		m_eOnCommit = ACTION_WRITE; break;
		default:	sphDie ( "unknown binlog flush mode %d (must be 0, 1, or 2)\n", iMode );
	}

#ifndef DATADIR
#define DATADIR "."
#endif

	m_sLogPath = hSearchd.GetStr ( "binlog_path", bTestMode ? "" : DATADIR );
	m_bDisabled = m_sLogPath.IsEmpty();

	m_iRestartSize = hSearchd.GetSize ( "binlog_max_log_size", m_iRestartSize );

	if ( !m_bDisabled )
	{
		LockFile ( true );
		LoadMeta();
	}
}

void RtBinlog_c::Replay ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags,
	ProgressCallbackSimple_t * pfnProgressCallback )
{
	if ( m_bDisabled || !hIndexes.GetLength() )
		return;

	// on replay started
	if ( pfnProgressCallback )
		pfnProgressCallback();

	int64_t tmReplay = sphMicroTimer();
	// do replay
	m_bReplayMode = true;
	int iLastLogState = 0;
	ARRAY_FOREACH ( i, m_dLogFiles )
	{
		iLastLogState = ReplayBinlog ( hIndexes, uReplayFlags, i );
		if ( pfnProgressCallback ) // on each replayed binlog
			pfnProgressCallback();
	}

	if ( m_dLogFiles.GetLength()>0 )
	{
		tmReplay = sphMicroTimer() - tmReplay;
		sphInfo ( "binlog: finished replaying total %d in %d.%03d sec",
			m_dLogFiles.GetLength(),
			(int)(tmReplay/1000000), (int)((tmReplay/1000)%1000) );
	}

	// FIXME?
	// in some cases, indexes might had been flushed during replay
	// and we might therefore want to update m_iFlushedTID everywhere
	// but for now, let's just wait until next flush for simplicity

	// resume normal operation
	m_bReplayMode = false;
	OpenNewLog ( iLastLogState );
}

void RtBinlog_c::GetFlushInfo ( BinlogFlushInfo_t & tFlush )
{
	if ( !m_bDisabled && m_eOnCommit!=ACTION_FSYNC )
	{
		m_iFlushTimeLeft = sphMicroTimer() + m_iFlushPeriod;
		tFlush.m_pLog = this;
		tFlush.m_fnWork = RtBinlog_c::DoAutoFlush;
	}
}

void RtBinlog_c::DoAutoFlush ( void * pBinlog )
{
	assert ( pBinlog );
	RtBinlog_c * pLog = (RtBinlog_c *)pBinlog;
	assert ( !pLog->m_bDisabled );

	if ( pLog->m_iFlushPeriod>0 && pLog->m_iFlushTimeLeft<sphMicroTimer() )
	{
		MEMORY ( MEM_BINLOG );

		pLog->m_iFlushTimeLeft = sphMicroTimer() + pLog->m_iFlushPeriod;

		if ( pLog->m_eOnCommit==ACTION_NONE || pLog->m_tWriter.HasUnwrittenData() )
		{
			Verify ( pLog->m_tWriteLock.Lock() );
			pLog->m_tWriter.Flush();
			Verify ( pLog->m_tWriteLock.Unlock() );
		}

		if ( pLog->m_tWriter.HasUnsyncedData() )
			pLog->m_tWriter.Fsync();
	}
}

int RtBinlog_c::GetWriteIndexID ( const char * sName, int64_t iTID, int64_t tmNow )
{
	MEMORY ( MEM_BINLOG );
	assert ( m_dLogFiles.GetLength() );

	// OPTIMIZE? maybe hash them?
	BinlogFileDesc_t & tLog = m_dLogFiles.Last();
	ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
	{
		BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];
		if ( tIndex.m_sName==sName )
		{
			tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
			tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmNow );
			return i;
		}
	}

	// create a new entry
	int iID = tLog.m_dIndexInfos.GetLength();
	BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos.Add(); // caller must hold a wlock
	tIndex.m_sName = sName;
	tIndex.m_iMinTID = iTID;
	tIndex.m_iMaxTID = iTID;
	tIndex.m_iFlushedTID = 0;
	tIndex.m_tmMin = tmNow;
	tIndex.m_tmMax = tmNow;

	// log this new entry
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_ADD_INDEX );
	m_tWriter.ZipOffset ( iID );
	m_tWriter.PutString ( sName );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );
	m_tWriter.WriteCrc ();

	// return the index
	return iID;
}

void RtBinlog_c::LoadMeta ()
{
	MEMORY ( MEM_BINLOG );

	CSphString sMeta;
	sMeta.SetSprintf ( "%s/binlog.meta", m_sLogPath.cstr() );
	if ( !sphIsReadable ( sMeta.cstr() ) )
		return;

	CSphString sError;

	// opened and locked, lets read
	CSphAutoreader rdMeta;
	if ( !rdMeta.Open ( sMeta, sError ) )
		sphDie ( "%s error: %s", sMeta.cstr(), sError.cstr() );

	if ( rdMeta.GetDword()!=BINLOG_META_MAGIC )
		sphDie ( "invalid meta file %s", sMeta.cstr() );

	// binlog meta v1 was dev only, crippled, and we don't like it anymore
	// binlog metas v2 upto current v4 (and likely up) share the same simplistic format
	// so let's support empty (!) binlogs w/ known versions and compatible metas
	DWORD uVersion = rdMeta.GetDword();
	if ( uVersion==1 || uVersion>BINLOG_VERSION )
		sphDie ( "binlog meta file %s is v.%d, binary is v.%d; recovery requires previous binary version",
			sMeta.cstr(), uVersion, BINLOG_VERSION );

	const bool bLoaded64bit = ( rdMeta.GetByte()==1 );
	m_dLogFiles.Resize ( rdMeta.UnzipInt() ); // FIXME! sanity check

	if ( !m_dLogFiles.GetLength() )
		return;

	// ok, so there is actual recovery data
	// let's require that exact version and bitness, then
	if ( uVersion!=BINLOG_VERSION )
		sphDie ( "binlog meta file %s is v.%d, binary is v.%d; recovery requires previous binary version",
			sMeta.cstr(), uVersion, BINLOG_VERSION );

	if ( !bLoaded64bit )
		sphDie ( "indexes with 32-bit docids are no longer supported; recovery requires previous binary version" );

	// load list of active log files
	ARRAY_FOREACH ( i, m_dLogFiles )
		m_dLogFiles[i].m_iExt = rdMeta.UnzipInt(); // everything else is saved in logs themselves
}

void RtBinlog_c::SaveMeta ()
{
	MEMORY ( MEM_BINLOG );

	CSphString sMeta, sMetaOld;
	sMeta.SetSprintf ( "%s/binlog.meta.new", m_sLogPath.cstr() );
	sMetaOld.SetSprintf ( "%s/binlog.meta", m_sLogPath.cstr() );

	CSphString sError;

	// opened and locked, lets write
	CSphWriter wrMeta;
	if ( !wrMeta.OpenFile ( sMeta, sError ) )
		sphDie ( "failed to open '%s': '%s'", sMeta.cstr(), sError.cstr() );

	wrMeta.PutDword ( BINLOG_META_MAGIC );
	wrMeta.PutDword ( BINLOG_VERSION );
	wrMeta.PutByte ( 1 ); // was USE_64BIT

	// save list of active log files
	wrMeta.ZipInt ( m_dLogFiles.GetLength() );
	ARRAY_FOREACH ( i, m_dLogFiles )
		wrMeta.ZipInt ( m_dLogFiles[i].m_iExt ); // everything else is saved in logs themselves

	wrMeta.CloseFile();

	if ( sph::rename ( sMeta.cstr(), sMetaOld.cstr() ) )
		sphDie ( "failed to rename meta (src=%s, dst=%s, errno=%d, error=%s)",
			sMeta.cstr(), sMetaOld.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully
	sphLogDebug ( "SaveMeta: Done." );
}

void RtBinlog_c::LockFile ( bool bLock )
{
	CSphString sName;
	sName.SetSprintf ( "%s/binlog.lock", m_sLogPath.cstr() );

	if ( bLock )
	{
		assert ( m_iLockFD==-1 );
		const int iLockFD = ::open ( sName.cstr(), SPH_O_NEW, 0644 );

		if ( iLockFD<0 )
			sphDie ( "failed to open '%s': %u '%s'", sName.cstr(), errno, strerrorm(errno) );

		if ( !sphLockEx ( iLockFD, false ) )
			sphDie ( "failed to lock '%s': %u '%s'", sName.cstr(), errno, strerrorm(errno) );

		m_iLockFD = iLockFD;
	} else
	{
		if ( m_iLockFD>=0 )
			sphLockUn ( m_iLockFD );
		SafeClose ( m_iLockFD );
		::unlink ( sName.cstr()	);
	}
}

void RtBinlog_c::OpenNewLog ( int iLastState )
{
	MEMORY ( MEM_BINLOG );

	// calc new ext
	int iExt = 1;
	if ( m_dLogFiles.GetLength() )
	{
		iExt = m_dLogFiles.Last().m_iExt;
		if ( !iLastState )
			iExt++;
	}

	// create entry
	// we need to reset it, otherwise there might be leftover data after last Remove()
	BinlogFileDesc_t tLog;
	tLog.m_iExt = iExt;
	m_dLogFiles.Add ( tLog );

	// create file
	CSphString sLog = MakeBinlogName ( m_sLogPath.cstr(), tLog.m_iExt );

	if ( !iLastState ) // reuse the last binlog since it is empty or useless.
		::unlink ( sLog.cstr() );

	if ( !m_tWriter.OpenFile ( sLog.cstr(), m_sWriterError ) )
		sphDie ( "failed to create %s: errno=%d, error=%s", sLog.cstr(), errno, strerrorm(errno) );

	// emit header
	m_tWriter.PutDword ( BINLOG_HEADER_MAGIC );
	m_tWriter.PutDword ( BINLOG_VERSION );

	// update meta
	SaveMeta();
}

void RtBinlog_c::DoCacheWrite ()
{
	if ( !m_dLogFiles.GetLength() )
		return;
	const CSphVector<BinlogIndexInfo_t> & dIndexes = m_dLogFiles.Last().m_dIndexInfos;

	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_ADD_CACHE );
	m_tWriter.ZipOffset ( dIndexes.GetLength() );
	ARRAY_FOREACH ( i, dIndexes )
	{
		m_tWriter.PutString ( dIndexes[i].m_sName.cstr() );
		m_tWriter.ZipOffset ( dIndexes[i].m_iMinTID );
		m_tWriter.ZipOffset ( dIndexes[i].m_iMaxTID );
		m_tWriter.ZipOffset ( dIndexes[i].m_iFlushedTID );
		m_tWriter.ZipOffset ( dIndexes[i].m_tmMin );
		m_tWriter.ZipOffset ( dIndexes[i].m_tmMax );
	}
	m_tWriter.WriteCrc ();
}

void RtBinlog_c::CheckDoRestart ()
{
	// restart on exceed file size limit
	if ( m_iRestartSize>0 && m_tWriter.GetPos()>m_iRestartSize )
	{
		MEMORY ( MEM_BINLOG );

		assert ( m_dLogFiles.GetLength() );

		DoCacheWrite();
		m_tWriter.CloseFile();
		OpenNewLog();
	}
}

void RtBinlog_c::CheckDoFlush ()
{
	if ( m_eOnCommit==ACTION_NONE )
		return;

	if ( m_eOnCommit==ACTION_WRITE && m_tWriter.HasUnwrittenData() )
		m_tWriter.Write();

	if ( m_eOnCommit==ACTION_FSYNC && m_tWriter.HasUnsyncedData() )
	{
		if ( m_tWriter.HasUnwrittenData() )
			m_tWriter.Write();

		m_tWriter.Fsync();
	}
}

int RtBinlog_c::ReplayBinlog ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, int iBinlog )
{
	assert ( iBinlog>=0 && iBinlog<m_dLogFiles.GetLength() );
	CSphString sError;

	const CSphString sLog ( MakeBinlogName ( m_sLogPath.cstr(), m_dLogFiles[iBinlog].m_iExt ) );
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];

	// open, check, play
	sphInfo ( "binlog: replaying log %s", sLog.cstr() );

	BinlogReader_c tReader;
	if ( !tReader.Open ( sLog, sError ) )
	{
		if ( ( uReplayFlags & SPH_REPLAY_IGNORE_OPEN_ERROR )!=0 )
		{
			sphWarning ( "binlog: log open error: %s", sError.cstr() );
			return 0;
		}
		sphDie ( "binlog: log open error: %s", sError.cstr() );
	}

	const SphOffset_t iFileSize = tReader.GetFilesize();

	if ( !iFileSize )
	{
		sphWarning ( "binlog: empty binlog %s detected, skipping", sLog.cstr() );
		return -1;
	}

	if ( tReader.GetDword()!=BINLOG_HEADER_MAGIC )
		sphDie ( "binlog: log %s missing magic header (corrupted?)", sLog.cstr() );

	DWORD uVersion = tReader.GetDword();
	if ( uVersion!=BINLOG_VERSION || tReader.GetErrorFlag() )
		sphDie ( "binlog: log %s is v.%d, binary is v.%d; recovery requires previous binary version", sLog.cstr(), uVersion, BINLOG_VERSION );

	/////////////
	// do replay
	/////////////

	int dTotal [ BLOP_TOTAL+1 ];
	memset ( dTotal, 0, sizeof(dTotal) );

	// !COMMIT
	// instead of simply replaying everything, we should check whether this binlog is clean
	// by loading and checking the cache stored at its very end
	tLog.m_dIndexInfos.Reset();

	bool bReplayOK = true;
	bool bHaveCacheOp = false;
	int64_t iPos = -1;

	m_iReplayedRows = 0;
	int64_t tmReplay = sphMicroTimer();

	while ( iFileSize!=tReader.GetPos() && !tReader.GetErrorFlag() && bReplayOK )
	{
		iPos = tReader.GetPos();
		if ( tReader.GetDword()!=BLOP_MAGIC )
		{
			sphDie ( "binlog: log missing txn marker at pos=" INT64_FMT " (corrupted?)", iPos );
			break;
		}

		tReader.ResetCrc ();
		const uint64_t uOp = tReader.UnzipOffset ();

		if ( uOp<=0 || uOp>=BLOP_TOTAL )
			sphDie ( "binlog: unexpected entry (blop=" UINT64_FMT ", pos=" INT64_FMT ")", uOp, iPos );

		// FIXME! blop might be OK but skipped (eg. index that is no longer)
		switch ( uOp )
		{
			case BLOP_COMMIT:
				bReplayOK = ReplayCommit ( iBinlog, uReplayFlags, tReader );
				break;

			case BLOP_UPDATE_ATTRS:
				bReplayOK = ReplayUpdateAttributes ( iBinlog, tReader );
				break;

			case BLOP_ADD_INDEX:
				bReplayOK = ReplayIndexAdd ( iBinlog, hIndexes, tReader );
				break;

			case BLOP_ADD_CACHE:
				if ( bHaveCacheOp )
					sphDie ( "binlog: internal error, second BLOP_ADD_CACHE detected (corruption?)" );
				bHaveCacheOp = true;
				bReplayOK = ReplayCacheAdd ( iBinlog, tReader );
				break;

			case BLOP_RECONFIGURE:
				bReplayOK = ReplayReconfigure ( iBinlog, uReplayFlags, tReader );
				break;

			default:
				sphDie ( "binlog: internal error, unhandled entry (blop=%d)", (int)uOp );
		}

		dTotal [ uOp ] += bReplayOK ? 1 : 0;
		dTotal [ BLOP_TOTAL ]++;
	}

	tmReplay = sphMicroTimer() - tmReplay;

	if ( tReader.GetErrorFlag() )
		sphWarning ( "binlog: log io error at pos=" INT64_FMT ": %s", iPos, sError.cstr() );

	if ( !bReplayOK )
		sphWarning ( "binlog: replay error at pos=" INT64_FMT ")", iPos );

	// show additional replay statistics
	ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
	{
		const BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];
		if ( !hIndexes ( tIndex.m_sName.cstr() ) )
		{
			sphWarning ( "binlog: index %s: missing; tids " INT64_FMT " to " INT64_FMT " skipped!",
				tIndex.m_sName.cstr(), tIndex.m_iMinTID, tIndex.m_iMaxTID );

		} else if ( tIndex.m_iPreReplayTID < tIndex.m_iMaxTID )
		{
			sphInfo ( "binlog: index %s: recovered from tid " INT64_FMT " to tid " INT64_FMT,
				tIndex.m_sName.cstr(), tIndex.m_iPreReplayTID, tIndex.m_iMaxTID );

		} else
		{
			sphInfo ( "binlog: index %s: skipped at tid " INT64_FMT " and max binlog tid " INT64_FMT,
				tIndex.m_sName.cstr(), tIndex.m_iPreReplayTID, tIndex.m_iMaxTID );
		}
	}

	sphInfo ( "binlog: replay stats: %d rows in %d commits; %d updates, %d reconfigure; %d indexes",
		m_iReplayedRows, dTotal[BLOP_COMMIT], dTotal[BLOP_UPDATE_ATTRS], dTotal[BLOP_RECONFIGURE], dTotal[BLOP_ADD_INDEX] );
	sphInfo ( "binlog: finished replaying %s; %d.%d MB in %d.%03d sec",
		sLog.cstr(),
		(int)(iFileSize/1048576), (int)((iFileSize*10/1048576)%10),
		(int)(tmReplay/1000000), (int)((tmReplay/1000)%1000) );

	if ( bHaveCacheOp && dTotal[BLOP_TOTAL]==1 ) // only one operation, that is Add Cache - by the fact, empty binlog
		return 1;

	return 0;
}


static BinlogIndexInfo_t & ReplayIndexID ( BinlogReader_c & tReader, BinlogFileDesc_t & tLog, const char * sPlace )
{
	const int64_t iTxnPos = tReader.GetPos();
	const int iVal = (int)tReader.UnzipOffset();

	if ( iVal<0 || iVal>=tLog.m_dIndexInfos.GetLength() )
		sphDie ( "binlog: %s: unexpected index id (id=%d, max=%d, pos=" INT64_FMT ")",
			sPlace, iVal, tLog.m_dIndexInfos.GetLength(), iTxnPos );

	return tLog.m_dIndexInfos[iVal];
}


bool RtBinlog_c::ReplayCommit ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "commit" );

	// load transaction data
	const int64_t iTID = (int64_t) tReader.UnzipOffset();
	const int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	CSphScopedPtr<RtSegment_t> pSeg ( NULL );
	CSphVector<SphDocID_t> dKlist;

	int iRows = (int)tReader.UnzipOffset();
	if ( iRows )
	{
		pSeg = new RtSegment_t();
		pSeg->m_iRows = pSeg->m_iAliveRows = iRows;
		m_iReplayedRows += iRows;

		LoadVector ( tReader, pSeg->m_dWords );
		pSeg->m_dWordCheckpoints.Resize ( (int) tReader.UnzipOffset() ); // FIXME! sanity check
		ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
		{
			pSeg->m_dWordCheckpoints[i].m_iOffset = (int) tReader.UnzipOffset();
			pSeg->m_dWordCheckpoints[i].m_uWordID = (SphWordID_t )tReader.UnzipOffset();
		}
		LoadVector ( tReader, pSeg->m_dDocs );
		LoadVector ( tReader, pSeg->m_dHits );
		LoadVector ( tReader, pSeg->m_dRows );
		LoadVector ( tReader, pSeg->m_dStrings );
		LoadVector ( tReader, pSeg->m_dMvas );
		LoadVector ( tReader, pSeg->m_dKeywordCheckpoints );
	}
	LoadVector ( tReader, dKlist );

	// checksum
	if ( tReader.GetErrorFlag() || !tReader.CheckCrc ( "commit", tIndex.m_sName.cstr(), iTID, iTxnPos ) )
		return false;

	// check TID
	if ( iTID<tIndex.m_iMaxTID )
		sphDie ( "binlog: commit: descending tid (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos );

	// check timestamp
	if ( tmStamp<tIndex.m_tmMax )
	{
		if (!( uReplayFlags & SPH_REPLAY_ACCEPT_DESC_TIMESTAMP ))
			sphDie ( "binlog: commit: descending time (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos );

		sphWarning ( "binlog: commit: replaying txn despite descending time "
			"(index=%s, logtid=" INT64_FMT ", lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), iTID, tIndex.m_tmMax, tmStamp, iTxnPos );
		tIndex.m_tmMax = tmStamp;
	}

	// only replay transaction when index exists and does not have it yet (based on TID)
	if ( tIndex.m_pRT && iTID > tIndex.m_pRT->m_iTID )
	{
		// we normally expect per-index TIDs to be sequential
		// but let's be graceful about that
		if ( iTID!=tIndex.m_pRT->m_iTID+1 )
			sphWarning ( "binlog: commit: unexpected tid (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_pRT->m_iTID, iTID, iTxnPos );

		// in case dict=keywords
		// + cook checkpoint
		// + build infixes
		if ( tIndex.m_pRT->IsWordDict() && pSeg.Ptr() )
		{
			FixupSegmentCheckpoints ( pSeg.Ptr() );
			BuildSegmentInfixes ( pSeg.Ptr(), tIndex.m_pRT->GetDictionary()->HasMorphology(),
				tIndex.m_pRT->IsWordDict(), tIndex.m_pRT->GetSettings().m_iMinInfixLen, tIndex.m_pRT->GetWordCheckoint(), ( tIndex.m_pRT->GetMaxCodepointLength()>1 ) );
		}

		// actually replay
		tIndex.m_pRT->CommitReplayable ( pSeg.LeakPtr(), dKlist, NULL );

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pRT->m_iTID = iTID;
	}

	// update info
	tIndex.m_iMinTID = Min ( tIndex.m_iMinTID, iTID );
	tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
	tIndex.m_tmMin = Min ( tIndex.m_tmMin, tmStamp );
	tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmStamp );
	return true;
}

bool RtBinlog_c::ReplayIndexAdd ( int iBinlog, const SmallStringHash_T<CSphIndex*> & hIndexes, BinlogReader_c & tReader ) const
{
	// load and check index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];

	uint64_t uVal = tReader.UnzipOffset();
	if ( (int)uVal!=tLog.m_dIndexInfos.GetLength() )
		sphDie ( "binlog: indexadd: unexpected index id (id=" UINT64_FMT ", expected=%d, pos=" INT64_FMT ")",
			uVal, tLog.m_dIndexInfos.GetLength(), iTxnPos );

	// load data
	CSphString sName = tReader.GetString();

	// FIXME? use this for double checking?
	tReader.UnzipOffset (); // TID
	tReader.UnzipOffset (); // time

	if ( !tReader.CheckCrc ( "indexadd", sName.cstr(), 0, iTxnPos ) )
		return false;

	// check for index name dupes
	ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
		if ( tLog.m_dIndexInfos[i].m_sName==sName )
			sphDie ( "binlog: duplicate index name (name=%s, dupeid=%d, pos=" INT64_FMT ")",
				sName.cstr(), i, iTxnPos );

	// not a dupe, lets add
	BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos.Add();
	tIndex.m_sName = sName;

	// lookup index in the list of currently served ones
	CSphIndex ** ppIndex = hIndexes ( sName.cstr() );
	CSphIndex * pIndex = ppIndex ? (*ppIndex) : NULL;
	if ( pIndex )
	{
		tIndex.m_pIndex = pIndex;
		if ( pIndex->IsRT() )
			tIndex.m_pRT = (RtIndex_t*)pIndex;
		tIndex.m_iPreReplayTID = pIndex->m_iTID;
		tIndex.m_iFlushedTID = pIndex->m_iTID;
	}

	// all ok
	// TID ranges will be now recomputed as we replay
	return true;
}

bool RtBinlog_c::ReplayUpdateAttributes ( int iBinlog, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "update" );

	// load transaction data
	CSphAttrUpdate tUpd;
	tUpd.m_bIgnoreNonexistent = true;

	int64_t iTID = (int64_t) tReader.UnzipOffset();
	int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	int iAttrs = (int)tReader.UnzipOffset();
	tUpd.m_dAttrs.Resize ( iAttrs ); // FIXME! sanity check
	tUpd.m_dTypes.Resize ( iAttrs ); // FIXME! sanity check
	ARRAY_FOREACH ( i, tUpd.m_dAttrs )
	{
		tUpd.m_dAttrs[i] = tReader.GetString().Leak();
		tUpd.m_dTypes[i] = (ESphAttr) tReader.UnzipOffset(); // safe, we'll crc check later
	}
	if ( tReader.GetErrorFlag()
		|| !LoadVector ( tReader, tUpd.m_dPool )
		|| !LoadVector ( tReader, tUpd.m_dDocids )
		|| !LoadVector ( tReader, tUpd.m_dRowOffset )
		|| !tReader.CheckCrc ( "update", tIndex.m_sName.cstr(), iTID, iTxnPos ) )
	{
		return false;
	}

	// check TID, time order in log
	if ( iTID<tIndex.m_iMaxTID )
		sphDie ( "binlog: update: descending tid (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos );
	if ( tmStamp<tIndex.m_tmMax )
		sphDie ( "binlog: update: descending time (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos );

	if ( tIndex.m_pIndex && iTID > tIndex.m_pIndex->m_iTID )
	{
		// we normally expect per-index TIDs to be sequential
		// but let's be graceful about that
		if ( iTID!=tIndex.m_pIndex->m_iTID+1 )
			sphWarning ( "binlog: update: unexpected tid (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_pIndex->m_iTID, iTID, iTxnPos );

		tUpd.m_dRows.Resize ( tUpd.m_dDocids.GetLength() );
		ARRAY_FOREACH ( i, tUpd.m_dRows ) tUpd.m_dRows[i] = NULL;

		CSphString sError, sWarning;
		tIndex.m_pIndex->UpdateAttributes ( tUpd, -1, sError, sWarning ); // FIXME! check for errors

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pIndex->m_iTID = iTID;
	}

	// update info
	tIndex.m_iMinTID = Min ( tIndex.m_iMinTID, iTID );
	tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
	tIndex.m_tmMin = Min ( tIndex.m_tmMin, tmStamp );
	tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmStamp );
	return true;
}

bool RtBinlog_c::ReplayCacheAdd ( int iBinlog, BinlogReader_c & tReader ) const
{
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];

	// load data
	CSphVector<BinlogIndexInfo_t> dCache;
	dCache.Resize ( (int) tReader.UnzipOffset() ); // FIXME! sanity check
	ARRAY_FOREACH ( i, dCache )
	{
		dCache[i].m_sName = tReader.GetString();
		dCache[i].m_iMinTID = tReader.UnzipOffset();
		dCache[i].m_iMaxTID = tReader.UnzipOffset();
		dCache[i].m_iFlushedTID = tReader.UnzipOffset();
		dCache[i].m_tmMin = tReader.UnzipOffset();
		dCache[i].m_tmMax = tReader.UnzipOffset();
	}
	if ( !tReader.CheckCrc ( "cache", "", 0, iTxnPos ) )
		return false;

	// if we arrived here by replay, let's verify everything
	// note that cached infos just passed checksumming, so the file is supposed to be clean!
	// in any case, broken log or not, we probably managed to replay something
	// so let's just report differences as warnings

	if ( dCache.GetLength()!=tLog.m_dIndexInfos.GetLength() )
	{
		sphWarning ( "binlog: cache mismatch: %d indexes cached, %d replayed",
			dCache.GetLength(), tLog.m_dIndexInfos.GetLength() );
		return true;
	}

	ARRAY_FOREACH ( i, dCache )
	{
		BinlogIndexInfo_t & tCache = dCache[i];
		BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];

		if ( tCache.m_sName!=tIndex.m_sName )
		{
			sphWarning ( "binlog: cache mismatch: index %d name mismatch (%s cached, %s replayed)",
				i, tCache.m_sName.cstr(), tIndex.m_sName.cstr() );
			continue;
		}

		if ( tCache.m_iMinTID!=tIndex.m_iMinTID || tCache.m_iMaxTID!=tIndex.m_iMaxTID )
		{
			sphWarning ( "binlog: cache mismatch: index %s tid ranges mismatch "
				"(cached " INT64_FMT " to " INT64_FMT ", replayed " INT64_FMT " to " INT64_FMT ")",
				tCache.m_sName.cstr(),
				tCache.m_iMinTID, tCache.m_iMaxTID, tIndex.m_iMinTID, tIndex.m_iMaxTID );
		}
	}

	return true;
}

bool RtBinlog_c::ReplayReconfigure ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "reconfigure" );

	// load transaction data
	const int64_t iTID = (int64_t) tReader.UnzipOffset();
	const int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	CSphString sError;
	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;

	CSphReconfigureSettings tSettings;
	LoadIndexSettings ( tSettings.m_tIndex, tReader, INDEX_FORMAT_VERSION );
	if ( !LoadTokenizerSettings ( tReader, tSettings.m_tTokenizer, tEmbeddedFiles, INDEX_FORMAT_VERSION, sError ) )
		sphDie ( "binlog: reconfigure: failed to load settings (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ", error=%s)",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos, sError.cstr() );
	LoadDictionarySettings ( tReader, tSettings.m_tDict, tEmbeddedFiles, INDEX_FORMAT_VERSION, sError );
	LoadFieldFilterSettings ( tReader, tSettings.m_tFieldFilter );

	// checksum
	if ( tReader.GetErrorFlag() || !tReader.CheckCrc ( "reconfigure", tIndex.m_sName.cstr(), iTID, iTxnPos ) )
		return false;

	// check TID
	if ( iTID<tIndex.m_iMaxTID )
		sphDie ( "binlog: reconfigure: descending tid (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos );

	// check timestamp
	if ( tmStamp<tIndex.m_tmMax )
	{
		if (!( uReplayFlags & SPH_REPLAY_ACCEPT_DESC_TIMESTAMP ))
			sphDie ( "binlog: reconfigure: descending time (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos );

		sphWarning ( "binlog: reconfigure: replaying txn despite descending time "
			"(index=%s, logtid=" INT64_FMT ", lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), iTID, tIndex.m_tmMax, tmStamp, iTxnPos );
		tIndex.m_tmMax = tmStamp;
	}

	// only replay transaction when index exists and does not have it yet (based on TID)
	if ( tIndex.m_pRT && iTID > tIndex.m_pRT->m_iTID )
	{
		// we normally expect per-index TIDs to be sequential
		// but let's be graceful about that
		if ( iTID!=tIndex.m_pRT->m_iTID+1 )
			sphWarning ( "binlog: reconfigure: unexpected tid (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_pRT->m_iTID, iTID, iTxnPos );

		sError = "";
		CSphReconfigureSetup tSetup;
		bool bSame = tIndex.m_pRT->IsSameSettings ( tSettings, tSetup, sError );

		if ( !sError.IsEmpty() )
			sphWarning ( "binlog: reconfigure: wrong settings (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ", error=%s)",
				tIndex.m_sName.cstr(), tIndex.m_pRT->m_iTID, iTID, iTxnPos, sError.cstr() );

		if ( !bSame )
			tIndex.m_pRT->Reconfigure ( tSetup );

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pRT->m_iTID = iTID;
	}

	// update info
	tIndex.m_iMinTID = Min ( tIndex.m_iMinTID, iTID );
	tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
	tIndex.m_tmMin = Min ( tIndex.m_tmMin, tmStamp );
	tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmStamp );
	return true;
}

void RtBinlog_c::CheckPath ( const CSphConfigSection & hSearchd, bool bTestMode )
{
#ifndef DATADIR
#define DATADIR "."
#endif

	m_sLogPath = hSearchd.GetStr ( "binlog_path", bTestMode ? "" : DATADIR );
	m_bDisabled = m_sLogPath.IsEmpty();

	if ( !m_bDisabled )
	{
		LockFile ( true );
		LockFile ( false );
	}
}

//////////////////////////////////////////////////////////////////////////

ISphRtIndex * sphGetCurrentIndexRT()
{
	ISphRtAccum * pAcc = (ISphRtAccum*) sphThreadGet ( g_tTlsAccumKey );
	if ( pAcc )
		return pAcc->GetIndex();
	return NULL;
}

ISphRtIndex * sphCreateIndexRT ( const CSphSchema & tSchema, const char * sIndexName,
	int64_t iRamSize, const char * sPath, bool bKeywordDict )
{
	MEMORY ( MEM_INDEX_RT );
	return new RtIndex_t ( tSchema, sIndexName, iRamSize, sPath, bKeywordDict );
}

void sphRTInit ( const CSphConfigSection & hSearchd, bool bTestMode, const CSphConfigSection * pCommon )
{
	MEMORY ( MEM_BINLOG );

	g_bRTChangesAllowed = false;
	Verify ( sphThreadKeyCreate ( &g_tTlsAccumKey ) );

	g_pRtBinlog = new RtBinlog_c();
	if ( !g_pRtBinlog )
		sphDie ( "binlog: failed to create binlog" );
	g_pBinlog = g_pRtBinlog;

	// check binlog path before detaching from the console
	g_pRtBinlog->CheckPath ( hSearchd, bTestMode );

	if ( pCommon )
		g_bProgressiveMerge = ( pCommon->GetInt ( "progressive_merge", 1 )!=0 );
}


void sphRTConfigure ( const CSphConfigSection & hSearchd, bool bTestMode )
{
	assert ( g_pBinlog );
	g_pRtBinlog->Configure ( hSearchd, bTestMode );
	g_iRtFlushPeriod = hSearchd.GetInt ( "rt_flush_period", (int)g_iRtFlushPeriod );
	g_iRtFlushPeriod = Max ( g_iRtFlushPeriod, 10 );
}


void sphRTDone ()
{
	sphThreadKeyDelete ( g_tTlsAccumKey );
	// its valid for "searchd --stop" case
	SafeDelete ( g_pBinlog );
}


void sphReplayBinlog ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, ProgressCallbackSimple_t * pfnProgressCallback, BinlogFlushInfo_t & tFlush )
{
	MEMORY ( MEM_BINLOG );
	g_pRtBinlog->Replay ( hIndexes, uReplayFlags, pfnProgressCallback );
	g_pRtBinlog->GetFlushInfo ( tFlush );
	g_bRTChangesAllowed = true;
}

static bool g_bTestMode = false;

void sphRTSetTestMode ()
{
	g_bTestMode = true;
}

bool sphRTSchemaConfigure ( const CSphConfigSection & hIndex, CSphSchema * pSchema, CSphString * pError, bool bSkipValidation )
{
	assert ( pSchema && pError );


	// fields
	SmallStringHash_T<BYTE> hFields;
	for ( CSphVariant * v=hIndex("rt_field"); v; v=v->m_pNext )
	{
		CSphString sFieldName = v->cstr();
		sFieldName.ToLower();
		pSchema->AddField ( sFieldName.cstr() );
		hFields.Add ( 1, sFieldName );
	}

	if ( !pSchema->GetFieldsCount() && !bSkipValidation )
	{
		pError->SetSprintf ( "no fields configured (use rt_field directive)" );
		return false;
	}

	if ( pSchema->GetFieldsCount()>SPH_MAX_FIELDS )
	{
		pError->SetSprintf ( "too many fields (fields=%d, max=%d)", pSchema->GetFieldsCount(), SPH_MAX_FIELDS );
		return false;
	}

	// attrs
	const int iNumTypes = 9;
	const char * sTypes[iNumTypes] = { "rt_attr_uint", "rt_attr_bigint", "rt_attr_timestamp", "rt_attr_bool", "rt_attr_float", "rt_attr_string", "rt_attr_json", "rt_attr_multi", "rt_attr_multi_64" };
	const ESphAttr iTypes[iNumTypes] = { SPH_ATTR_INTEGER, SPH_ATTR_BIGINT, SPH_ATTR_TIMESTAMP, SPH_ATTR_BOOL, SPH_ATTR_FLOAT, SPH_ATTR_STRING, SPH_ATTR_JSON, SPH_ATTR_UINT32SET, SPH_ATTR_INT64SET };

	for ( int iType=0; iType<iNumTypes; ++iType )
	{
		for ( CSphVariant * v = hIndex ( sTypes[iType] ); v; v = v->m_pNext )
		{
			StrVec_t dNameParts;
			sphSplit ( dNameParts, v->cstr(), ":");
			CSphColumnInfo tCol ( dNameParts[0].cstr(), iTypes[iType]);
			tCol.m_sName.ToLower();

			// bitcount
			tCol.m_tLocator = CSphAttrLocator();
			if ( dNameParts.GetLength ()>1 )
			{
				if ( tCol.m_eAttrType==SPH_ATTR_INTEGER )
				{
					auto iBits = strtol ( dNameParts[1].cstr(), NULL, 10 );
					if ( iBits>0 && iBits<=ROWITEM_BITS )
						tCol.m_tLocator.m_iBitCount = (int)iBits;
					else
						pError->SetSprintf ( "attribute '%s': invalid bitcount=%d (bitcount ignored)", tCol.m_sName.cstr(), (int)iBits );

				} else
					pError->SetSprintf ( "attribute '%s': bitcount is only supported for integer types (bitcount ignored)", tCol.m_sName.cstr() );
			}

			pSchema->AddAttr ( tCol, false );

			if ( tCol.m_eAttrType!=SPH_ATTR_STRING && hFields.Exists ( tCol.m_sName ) && !bSkipValidation )
			{
				pError->SetSprintf ( "can not add attribute that shadows '%s' field", tCol.m_sName.cstr () );
				return false;
			}
		}
	}

	if ( !pSchema->GetAttrsCount() && !g_bTestMode && !bSkipValidation )
	{
		pError->SetSprintf ( "no attribute configured (use rt_attr directive)" );
		return false;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// percolate index


struct DictTerm_t
{
	SphWordID_t m_uWordID = 0;
	int m_iWordOff = 0;
	int m_iWordLen = 0;
};

struct DictMap_t
{
	CSphHash<DictTerm_t>	m_hTerms;
	CSphVector<BYTE>		m_dKeywords;

	SphWordID_t GetTerm ( BYTE * pWord ) const;
};

struct StoredQuery_t : ISphNoncopyable
{
	XQQuery_t *						m_pXQ = nullptr;

	CSphVector<uint64_t>			m_dRejectTerms;
	CSphFixedVector<uint64_t>		m_dRejectWilds {0};
	bool							m_bOnlyTerms = false; // flag of simple query, ie only words and no operators
	CSphVector<uint64_t>			m_dTags;
	CSphVector<CSphFilterSettings>	m_dFilters;
	CSphVector<FilterTreeItem_t>	m_dFilterTree;
	DictMap_t						m_hDict;
	CSphVector<CSphString>			m_dSuffixes;

	uint64_t						m_uUID = 0;
	// show status info
	CSphString						m_sQuery;
	CSphString						m_sTags;
	bool							m_bQL = true;
	bool							IsFullscan() const { return m_pXQ->m_bEmpty; }

	~StoredQuery_t() { SafeDelete ( m_pXQ ); }
};

static bool NotImplementedError ( CSphString * pError )
{
	if ( pError )
		*pError = "not implemented";

	return false;
}

struct StoredQueryKey_t
{
	uint64_t m_uUID;
	StoredQuery_t * m_pQuery;
};

// FindSpan vector operators
static bool operator < ( const StoredQueryKey_t & tKey, uint64_t uUID )
{
	return tKey.m_uUID<uUID;
}

static bool operator == ( const StoredQueryKey_t & tKey, uint64_t uUID )
{
	return tKey.m_uUID==uUID;
}

static bool operator < ( uint64_t uUID, const StoredQueryKey_t & tKey )
{
	return uUID<tKey.m_uUID;
}

static int g_iPercolateThreads = 1;

class PercolateIndex_c : public PercolateIndex_i
{
public:
	explicit PercolateIndex_c ( const CSphSchema & tSchema, const char * sIndexName, const char * sPath );
	~PercolateIndex_c () override;

	bool AddDocument ( ISphTokenizer * pTokenizer, int iFields, const char ** ppFields, const CSphMatch & tDoc, bool bReplace, const CSphString & sTokenFilterOptions, const char ** ppStr, const CSphVector<DWORD> & dMvas, CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt ) override;
	bool MatchDocuments ( ISphRtAccum * pAccExt, PercolateMatchResult_t &tRes ) override;
	void RollBack ( ISphRtAccum * pAccExt ) override;
	bool AddQuery ( const char * sQuery, const char * sTags, const CSphVector<CSphFilterSettings> * pFilters, const CSphVector<FilterTreeItem_t> * pFilterTree, bool bReplace, bool bQL, uint64_t & uId, const ISphTokenizer * pTokenizer, CSphDict * pDict, CSphString & sError )
		REQUIRES (!m_tLock);
	int DeleteQueries ( const uint64_t * pQueries, int iCount ) override;
	int DeleteQueries ( const char * sTags ) override;
	bool Query ( const char * sQuery, const char * sTags, const CSphVector<CSphFilterSettings> * pFilters, const CSphVector<FilterTreeItem_t> * pFilterTree, bool bReplace, bool bQL, uint64_t & uId, CSphString & sError ) override;
	bool Prealloc ( bool bStripPath ) override;
	void Dealloc () override {}
	void Preread () override {}
	void PostSetup() override;
	ISphRtAccum * CreateAccum ( CSphString & sError ) override;
	ISphTokenizer * CloneIndexingTokenizer() const override { return m_pTokenizerIndexing->Clone ( SPH_CLONE_INDEX ); }
	void SaveMeta ();
	void GetQueries ( const char * sFilterTags, bool bTagsEq, const CSphFilterSettings * pUID, int iOffset, int iLimit, CSphVector<PercolateQueryDesc> & dQueries ) override
		REQUIRES (!m_tLock);
	bool Truncate ( CSphString & ) override;

	// RT index stub
	bool MultiQuery ( const CSphQuery *, CSphQueryResult *, int, ISphMatchSorter **, const CSphMultiQueryArgs & ) const override;
	bool MultiQueryEx ( int, const CSphQuery *, CSphQueryResult **, ISphMatchSorter **, const CSphMultiQueryArgs & ) const override;
	virtual bool AddDocument ( ISphHits * , const CSphMatch & , const char ** , const CSphVector<DWORD> & , CSphString & , CSphString & ) { return true; }
	void Commit ( int * , ISphRtAccum * pAccExt ) override;
	bool DeleteDocument ( const SphDocID_t * , int , CSphString & , ISphRtAccum * pAccExt ) final;
	void CheckRamFlush () override {}
	void ForceRamFlush ( bool bPeriodic ) override;
	void ForceDiskChunk () override;
	bool AttachDiskIndex ( CSphIndex * , CSphString & ) override { return true; }
	void Optimize () override {}
	bool IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError ) const override;
	void Reconfigure ( CSphReconfigureSetup & tSetup ) override REQUIRES ( !m_tLock );
	CSphIndex * GetDiskChunk ( int ) override { return NULL; } // NOLINT
	int64_t GetFlushAge() const override { return 0; }

	// plain index stub
	SphDocID_t *		GetKillList () const override { return NULL; }
	int					GetKillListSize () const override { return 0 ; }
	bool				HasDocid ( SphDocID_t ) const override { return false; }
	int					Build ( const CSphVector<CSphSource*> & , int , int ) override { return 0; }
	bool				Merge ( CSphIndex * , const CSphVector<CSphFilterSettings> & , bool ) override {return false; }
	void				SetBase ( const char * ) override {}
	bool				Rename ( const char * ) override { return false; }
	bool				Lock () override { return true; }
	void				Unlock () override {}
//	virtual bool				Mlock () { return false; }
	bool				EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const override;
	const CSphSourceStats &	GetStats () const override { return m_tStat; }
	void				GetStatus ( CSphIndexStatus* pRes ) const override { assert (pRes); if ( pRes ) { pRes->m_iDiskUse = 0; pRes->m_iRamUse = 0;}}
	bool				GetKeywords ( CSphVector <CSphKeywordInfo> & , const char * , const GetKeywordsSettings_t & , CSphString * pError ) const override { return NotImplementedError(pError); }
	bool				FillKeywords ( CSphVector <CSphKeywordInfo> & ) const override { return false; }
	int					UpdateAttributes ( const CSphAttrUpdate & , int , CSphString & sError, CSphString & ) override { NotImplementedError ( &sError ); return -1; }
	bool				SaveAttributes ( CSphString & ) const override { return true; }
	DWORD				GetAttributeStatus () const override { return 0; }
	virtual bool		CreateModifiedFiles ( bool , const CSphString & , ESphAttr , int , CSphString & ) { return true; }
	bool				AddRemoveAttribute ( bool , const CSphString & , ESphAttr , CSphString & sError ) override { return NotImplementedError ( &sError ); }
	void				DebugDumpHeader ( FILE *, const char *, bool ) override {}
	void				DebugDumpDocids ( FILE * ) override {}
	void				DebugDumpHitlist ( FILE * , const char * , bool ) override {}
	int					DebugCheck ( FILE * ) override { return 0; } // NOLINT
	void				DebugDumpDict ( FILE * ) override {}
	void				SetProgressCallback ( CSphIndexProgress::IndexingProgress_fn ) override {}
	void				SetMemorySettings ( bool , bool , bool ) override {}

	const CSphSchema &GetMatchSchema () const override { return m_tMatchSchema; }

private:
	static const DWORD				META_HEADER_MAGIC = 0x50535451;	///< magic 'PSTQ' header
	static const DWORD				META_VERSION = 6;				///< current version, added expression filter

	int								m_iLockFD = -1;
	int								m_iDeleted = 0; // set in DeleteDocument, reset and return in Commit
	CSphSourceStats					m_tStat;
	ISphTokenizerRefPtr_c			m_pTokenizerIndexing;
	int								m_iMaxCodepointLength = 0;
	int64_t							m_iSavedTID = 1;
	int64_t							m_tmSaved = 0;

	CSphVector<StoredQueryKey_t>	m_dStored GUARDED_BY ( m_tLock );
	RwLock_t						m_tLock;

	CSphFixedVector<StoredQuery_t>	m_dLoadedQueries { 0 };
	CSphSchema						m_tMatchSchema;

	void DoMatchDocuments ( const RtSegment_t * pSeg, PercolateMatchResult_t & tRes ) REQUIRES ( !m_tLock );
	bool MultiScan ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters,
		ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs &tArgs ) const;
};

//////////////////////////////////////////////////////////////////////////
// percolate functions

#define PERCOLATE_BLOOM_WILD_COUNT 32
#define PERCOLATE_BLOOM_SIZE PERCOLATE_BLOOM_WILD_COUNT * 2
#define PERCOLATE_WORDS_PER_CP 128

/// percolate query index factory
PercolateIndex_i * CreateIndexPercolate ( const CSphSchema & tSchema, const char * sIndexName, const char * sPath )
{
	MEMORY ( MEM_INDEX_RT );
	return new PercolateIndex_c ( tSchema, sIndexName, sPath );
}

struct SegmentReject_t
{
	CSphVector<uint64_t> m_dTerms;
	CSphFixedVector<uint64_t> m_dWilds { 0 };
	CSphFixedVector< CSphVector<uint64_t> > m_dPerDocTerms { 0 };
	CSphFixedVector<uint64_t> m_dPerDocWilds { 0 };
	int m_iRows = 0;

	bool Filter ( const StoredQuery_t * pStored, bool bUtf8 ) const;
};

static void SegmentGetRejects ( const RtSegment_t * pSeg, bool bBuildInfix, bool bUtf8, SegmentReject_t & tReject )
{
	tReject.m_iRows = pSeg->m_iRows;
	const bool bMultiDocs = ( pSeg->m_iRows>1 );
	if ( bMultiDocs )
	{
		tReject.m_dPerDocTerms.Reset ( pSeg->m_iRows );
		if ( bBuildInfix )
		{
			tReject.m_dPerDocWilds.Reset ( pSeg->m_iRows * PERCOLATE_BLOOM_SIZE );
			tReject.m_dPerDocWilds.Fill ( 0 );
		}
	}
	if ( bBuildInfix )
	{
		tReject.m_dWilds.Reset ( PERCOLATE_BLOOM_SIZE );
		tReject.m_dWilds.Fill ( 0 );
	}

	RtWordReader_t tDict ( pSeg, true, PERCOLATE_WORDS_PER_CP );
	const RtWord_t * pWord = nullptr;
	BloomGenTraits_t tBloom0 ( tReject.m_dWilds.Begin() );
	BloomGenTraits_t tBloom1 ( tReject.m_dWilds.Begin() + PERCOLATE_BLOOM_WILD_COUNT );

	while ( ( pWord = tDict.UnzipWord() )!=nullptr )
	{
		const BYTE * pDictWord = pWord->m_sWord + 1;
		int iLen = pWord->m_sWord[0];

		uint64_t uHash = sphFNV64 ( pDictWord, iLen );
		tReject.m_dTerms.Add ( uHash );

		if ( bBuildInfix )
		{
			BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_0, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom0 );
			BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_1, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom1 );
		}

		if ( bMultiDocs )
		{
				RtDocReader_t tDoc ( pSeg, *pWord );
				while ( true )
				{
					const RtDoc_t * pDoc = tDoc.UnzipDoc();
					if ( !pDoc )
						break;

					// Docid - should be row-based started from 1
					assert ( pDoc->m_uDocID>=1 && (int)pDoc->m_uDocID<pSeg->m_iRows+1 );
					int iDoc = (int)pDoc->m_uDocID - 1;
					tReject.m_dPerDocTerms[iDoc].Add ( uHash );

					if ( bBuildInfix )
					{
						uint64_t * pBloom = tReject.m_dPerDocWilds.Begin() + iDoc * PERCOLATE_BLOOM_SIZE;
						BloomGenTraits_t tBloom2Doc0 ( pBloom );
						BloomGenTraits_t tBloom2Doc1 ( pBloom + PERCOLATE_BLOOM_WILD_COUNT );
						BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_0, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom2Doc0 );
						BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_1, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom2Doc1 );
					}
				}
		}
	}

	tReject.m_dTerms.Uniq();
	if ( bMultiDocs )
	{
		for ( auto & dTerms : tReject.m_dPerDocTerms )
			dTerms.Uniq();
	}
}

static void DoQueryGetRejects ( const XQNode_t * pNode, CSphDict * pDict, CSphVector<uint64_t> & dRejectTerms, CSphFixedVector<uint64_t> & dRejectBloom, CSphVector<CSphString> & dSuffixes, bool & bOnlyTerms, bool bUtf8 )
{
	// FIXME!!! replace recursion to prevent stack overflow for large and complex queries
	if ( pNode && !( pNode->GetOp()==SPH_QUERY_AND || pNode->GetOp()==SPH_QUERY_ANDNOT ) )
		bOnlyTerms = false;

	if ( !pNode || pNode->GetOp()==SPH_QUERY_NOT )
		return;

	BYTE sTmp[3 * SPH_MAX_WORD_LEN + 16];
	ARRAY_FOREACH ( i, pNode->m_dWords )
	{
		const XQKeyword_t & tWord = pNode->m_dWords[i];
		int iLen = tWord.m_sWord.Length();
		assert ( iLen < (int)sizeof( sTmp ) );

		if ( !iLen )
			continue;

		bool bStarTerm = false;
		int iCur = 0;
		int iInfixLen = 0;
		const char * sInfix = NULL;
		const char * s = tWord.m_sWord.cstr();
		BYTE * sDst = sTmp;

		while ( *s )
		{
			if ( sphIsWild ( *s ) )
			{
				iCur = 0;
				bStarTerm = true;
			} else if ( ++iCur>iInfixLen )
			{
				sInfix = s - iCur + 1;
				iInfixLen = iCur;
			}
			*sDst++ = *s++;
		}
		sTmp[iLen] = '\0';

		// term goes to bloom
		if ( bStarTerm )
		{
			// initialize bloom filter array
			if ( !dRejectBloom.GetLength() )
			{
				dRejectBloom.Reset ( PERCOLATE_BLOOM_SIZE );
				dRejectBloom.Fill ( 0 );
			}

			BloomGenTraits_t tBloom0 ( dRejectBloom.Begin() );
			BloomGenTraits_t tBloom1 ( dRejectBloom.Begin() + PERCOLATE_BLOOM_WILD_COUNT );
			BuildBloom ( (const BYTE *)sInfix, iInfixLen, BLOOM_NGRAM_0, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom0 );
			BuildBloom ( (const BYTE *)sInfix, iInfixLen, BLOOM_NGRAM_1, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom1 );
			dSuffixes.Add().SetBinary ( sInfix, iInfixLen );

			continue;
		}

		SphWordID_t uWord = 0;
		if ( tWord.m_bMorphed )
			uWord = pDict->GetWordIDNonStemmed ( sTmp );
		else
			uWord = pDict->GetWordID ( sTmp );

		if ( !uWord )
			continue;

		// term goes to regular array
		dRejectTerms.Add ( sphFNV64 ( sTmp ) );
	}


	// composite nodes children recursion
	// for AND-NOT node NOT children should be skipped
	int iCount = pNode->m_dChildren.GetLength();
	if ( pNode->GetOp()==SPH_QUERY_ANDNOT && iCount>1 )
		iCount = 1;
	for ( int i=0; i<iCount; i++ )
		DoQueryGetRejects ( pNode->m_dChildren[i], pDict, dRejectTerms, dRejectBloom, dSuffixes, bOnlyTerms, bUtf8 );
}

static void QueryGetRejects ( const XQNode_t * pNode, CSphDict * pDict, CSphVector<uint64_t> & dRejectTerms, CSphFixedVector<uint64_t> & dRejectBloom, CSphVector<CSphString> & dSuffixes, bool & bOnlyTerms, bool bUtf8 )
{
	DoQueryGetRejects ( pNode, pDict, dRejectTerms, dRejectBloom, dSuffixes, bOnlyTerms, bUtf8 );
	dRejectTerms.Uniq();
}

static void QueryGetTerms ( const XQNode_t * pNode, CSphDict * pDict, DictMap_t & hDict )
{
	if ( !pNode )
		return;

	BYTE sTmp[3 * SPH_MAX_WORD_LEN + 16];
	ARRAY_FOREACH ( i, pNode->m_dWords )
	{
		const XQKeyword_t & tWord = pNode->m_dWords[i];
		uint64_t uHash = sphFNV64 ( tWord.m_sWord.cstr() );
		if ( hDict.m_hTerms.Find ( uHash ) )
			continue;

		int iLen = tWord.m_sWord.Length();
		assert ( iLen < (int)sizeof( sTmp ) );

		if ( !iLen )
			continue;

		strncpy ( (char *)sTmp, tWord.m_sWord.cstr(), iLen );
		sTmp[iLen] = '\0';

		SphWordID_t uWord = 0;
		if ( tWord.m_bMorphed )
			uWord = pDict->GetWordIDNonStemmed ( sTmp );
		else
			uWord = pDict->GetWordID ( sTmp );

		if ( !uWord )
			continue;

		iLen = strnlen ( (const char *)sTmp, sizeof(sTmp) );
		DictTerm_t & tTerm = hDict.m_hTerms.Acquire ( uHash );
		tTerm.m_uWordID = uWord;
		tTerm.m_iWordOff = hDict.m_dKeywords.GetLength();
		tTerm.m_iWordLen = iLen;

		hDict.m_dKeywords.Append ( sTmp, iLen );
	}

	ARRAY_FOREACH ( i, pNode->m_dChildren )
		QueryGetTerms ( pNode->m_dChildren[i], pDict, hDict );
}

static bool TermsReject ( const CSphVector<uint64_t> & dDocs, const CSphVector<uint64_t> & dQueries )
{
	if ( !dDocs.GetLength() || !dQueries.GetLength() )
		return false;

	const uint64_t * pQTerm = dQueries.Begin();
	const uint64_t * pQEnd = dQueries.Begin() + dQueries.GetLength();
	const uint64_t * pTermDoc = dDocs.Begin();
	const uint64_t * pTermLast = dDocs.Begin() + dDocs.GetLength() - 1;

	for ( ; pQTerm<pQEnd && pTermDoc<=pTermLast; pQTerm++, pTermDoc++ )
	{
		pTermDoc = sphBinarySearch ( pTermDoc, pTermLast, *pQTerm );
		if ( !pTermDoc )
			return false;
	}

	return ( pQTerm==pQEnd );
}

static bool WildsReject ( const uint64_t * pFilter, const CSphFixedVector<uint64_t> & dQueries )
{
	if ( !dQueries.GetLength() )
		return false;

	const uint64_t * pQTerm = dQueries.Begin();
	const uint64_t * pQEnd = dQueries.Begin() + dQueries.GetLength();

	for ( ; pQTerm<pQEnd; pQTerm++, pFilter++ )
	{
		// check bloom passes
		if ( *pQTerm && ( (*pQTerm & *pFilter)!=*pQTerm ) )
			return false;
	}
	return true;
}

bool SegmentReject_t::Filter ( const StoredQuery_t * pStored, bool bUtf8 ) const
{
	// no early reject for complex queries
	if ( !pStored->m_bOnlyTerms )
		return false;

	// empty query rejects
	if ( !pStored->m_dRejectTerms.GetLength() && !pStored->m_dRejectWilds.GetLength() )
		return true;

	bool bTermsRejected = ( pStored->m_dRejectTerms.GetLength()==0 );
	if ( pStored->m_dRejectTerms.GetLength() )
		bTermsRejected = !TermsReject ( m_dTerms, pStored->m_dRejectTerms );

	if ( bTermsRejected && ( !m_dWilds.GetLength() || !pStored->m_dRejectWilds.GetLength() ) )
		return true;

	bool bWildRejected = ( m_dWilds.GetLength()==0 || pStored->m_dRejectWilds.GetLength()==0 );
	if ( m_dWilds.GetLength() && pStored->m_dRejectWilds.GetLength() )
		bWildRejected = !WildsReject ( m_dWilds.Begin(), pStored->m_dRejectWilds );

	if ( bTermsRejected && bWildRejected )
		return true;

	if ( !bTermsRejected && pStored->m_dRejectTerms.GetLength() && m_dPerDocTerms.GetLength() )
	{
		// in case no document matched - early reject triggers
		int iRejects = 0;
		ARRAY_FOREACH ( i, m_dPerDocTerms )
		{
			if ( TermsReject ( m_dPerDocTerms[i], pStored->m_dRejectTerms ) )
				break;
			
			iRejects++;
		}

		bTermsRejected = ( iRejects==m_dPerDocTerms.GetLength() );
	}

	if ( bTermsRejected && !bWildRejected && pStored->m_dRejectWilds.GetLength() && m_dPerDocWilds.GetLength() )
	{
		// in case no document matched - early reject triggers
		int iRowsPassed = 0;
		for ( int i=0; i<m_iRows && iRowsPassed==0; i++ )
		{
			BloomCheckTraits_t tBloom0 ( m_dPerDocWilds.Begin() + i * PERCOLATE_BLOOM_SIZE );
			BloomCheckTraits_t tBloom1 ( m_dPerDocWilds.Begin() + i * PERCOLATE_BLOOM_SIZE + PERCOLATE_BLOOM_WILD_COUNT );
			int iWordsPassed = 0;
			ARRAY_FOREACH ( iWord, pStored->m_dSuffixes )
			{
				const CSphString & sSuffix = pStored->m_dSuffixes[iWord];
				int iLen = sSuffix.Length();

				BuildBloom ( (const BYTE *)sSuffix.cstr(), iLen, BLOOM_NGRAM_0, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom0 );
				if ( !tBloom0.IterateNext() )
					break;
				BuildBloom ( (const BYTE *)sSuffix.cstr(), iLen, BLOOM_NGRAM_1, bUtf8, PERCOLATE_BLOOM_WILD_COUNT, tBloom1 );
				if ( !tBloom1.IterateNext() )
					break;

				iWordsPassed++;
			}
			if ( iWordsPassed!=pStored->m_dSuffixes.GetLength() )
				continue;
			
			iRowsPassed++;
		}

		bWildRejected = ( iRowsPassed==0 );
	}

	return ( bTermsRejected && bWildRejected );
}

// FIXME!!! move to common RT code instead copy-paste it
struct SubstringInfo_t
{
	char			m_sMorph[SPH_MAX_KEYWORD_LEN];
	const char *	m_sSubstring;
	const char *	m_sWildcard;
	int				m_iSubLen;

	SubstringInfo_t ()
		: m_sSubstring ( NULL )
		, m_sWildcard ( NULL )
		, m_iSubLen ( 0 )
	{}
};

static Slice_t GetTermLocator ( const char * sWord, int iLen, const RtSegment_t * pSeg )
{
	Slice_t tChPoint;
	tChPoint.m_uLen = pSeg->m_dWords.GetLength();

	// tighten dictionary location
	if ( pSeg->m_dWordCheckpoints.GetLength() )
	{
		const RtWordCheckpoint_t * pCheckpoint = sphSearchCheckpoint ( sWord, iLen, 0, false, true, pSeg->m_dWordCheckpoints.Begin(), &pSeg->m_dWordCheckpoints.Last() );
		if ( !pCheckpoint )
		{
			tChPoint.m_uLen = pSeg->m_dWordCheckpoints.Begin()->m_iOffset;
		} else
		{
			tChPoint.m_uOff = pCheckpoint->m_iOffset;
			if ( ( pCheckpoint + 1 )<=( &pSeg->m_dWordCheckpoints.Last() ) )
				tChPoint.m_uLen = pCheckpoint[1].m_iOffset;
		}
	}

	return tChPoint;
}

static Slice_t GetPrefixLocator ( const char * sWord, bool bHasMorphology, const RtSegment_t * pSeg, SubstringInfo_t & tSubInfo )
{
	// do prefix expansion
	// remove exact form modifier, if any
	const char * sPrefix = sWord;
	if ( *sPrefix=='=' )
		sPrefix++;

	// skip leading wild-cards
	// (in case we got here on non-infix index path)
	const char * sWildcard = sPrefix;
	while ( sphIsWild ( *sPrefix ) )
	{
		sPrefix++;
		sWildcard++;
	}

	// compute non-wild-card prefix length
	int iPrefix = 0;
	for ( const char * s = sPrefix; *s && !sphIsWild ( *s ); s++ )
		iPrefix++;

	// prefix expansion should work on non-stemmed words only
	if ( bHasMorphology )
	{
		tSubInfo.m_sMorph[0] = MAGIC_WORD_HEAD_NONSTEMMED;
		memcpy ( tSubInfo.m_sMorph + 1, sPrefix, iPrefix );
		sPrefix = tSubInfo.m_sMorph;
		iPrefix++;
	}

	tSubInfo.m_sWildcard = sWildcard;
	tSubInfo.m_sSubstring = sPrefix;
	tSubInfo.m_iSubLen = iPrefix;

	Slice_t tChPoint;
	tChPoint.m_uLen = pSeg->m_dWords.GetLength();

	// find initial checkpoint or check words prior to 1st checkpoint
	if ( pSeg->m_dWordCheckpoints.GetLength() )
	{
		const RtWordCheckpoint_t * pLast = &pSeg->m_dWordCheckpoints.Last();
		const RtWordCheckpoint_t * pCheckpoint = sphSearchCheckpoint ( sPrefix, iPrefix, 0, true, true, pSeg->m_dWordCheckpoints.Begin(), pLast );

		if ( pCheckpoint )
		{
			// there could be valid data prior 1st checkpoint that should be unpacked and checked
			int iNameLen = strnlen ( pCheckpoint->m_sWord, SPH_MAX_KEYWORD_LEN );
			if ( pCheckpoint!=pSeg->m_dWordCheckpoints.Begin() || (sphDictCmp ( sPrefix, iPrefix, pCheckpoint->m_sWord, iNameLen )==0 && iPrefix==iNameLen) )
				tChPoint.m_uOff = pCheckpoint->m_iOffset;

			// find the last checkpoint that meets prefix condition ( ie might be a span of terms that splat to a couple of checkpoints )
			pCheckpoint++;
			while ( pCheckpoint<=pLast )
			{
				iNameLen = strnlen ( pCheckpoint->m_sWord, SPH_MAX_KEYWORD_LEN );
				int iCmp = sphDictCmp ( sPrefix, iPrefix, pCheckpoint->m_sWord, iNameLen );
				if ( iCmp==0 && iPrefix==iNameLen )
					tChPoint.m_uOff = pCheckpoint->m_iOffset;
				if ( iCmp<0 )
					break;
				pCheckpoint++;
			}
		}
	}

	return tChPoint;
}

static void GetSuffixLocators ( const char * sWord, int iMaxCodepointLength, const RtSegment_t * pSeg, SubstringInfo_t & tSubInfo, CSphVector<Slice_t> & dPoints )
{
	assert ( sphIsWild ( *sWord ) );

	// find the longest substring of non-wild-cards
	const char * sMaxInfix = NULL;
	int iMaxInfix = 0;
	int iCur = 0;

	for ( const char * s = sWord; *s; s++ )
	{
		if ( sphIsWild ( *s ) )
		{
			iCur = 0;
		} else if ( ++iCur>iMaxInfix )
		{
			sMaxInfix = s - iCur + 1;
			iMaxInfix = iCur;
		}
	}

	tSubInfo.m_sWildcard = sWord;
	tSubInfo.m_sSubstring = sMaxInfix;
	tSubInfo.m_iSubLen = iMaxInfix;

	CSphVector<DWORD> dInfixes;
	ExtractInfixCheckpoints ( sMaxInfix, iMaxInfix, iMaxCodepointLength, pSeg->m_dWordCheckpoints.GetLength(), pSeg->m_dInfixFilterCP, dInfixes );

	ARRAY_FOREACH ( i, dInfixes )
	{
		int iNext = dInfixes[i];
		iCur = iNext - 1;

		Slice_t & tChPoint = dPoints.Add();
		tChPoint.m_uOff = 0;
		tChPoint.m_uLen = pSeg->m_dWords.GetLength();

		if ( iCur > 0 )
			tChPoint.m_uOff = pSeg->m_dWordCheckpoints[iCur].m_iOffset;
		if ( iNext < pSeg->m_dWordCheckpoints.GetLength() )
			tChPoint.m_uLen = pSeg->m_dWordCheckpoints[iNext].m_iOffset;
	}
}

static void PercolateTags ( const char * sTags, CSphVector<uint64_t> & dTags )
{
	if ( !sTags || !*sTags )
		return;

	StrVec_t dTagStrings;
	sphSplit ( dTagStrings, sTags );
	if ( !dTagStrings.GetLength() )
		return;

	dTags.Resize ( dTagStrings.GetLength() );
	ARRAY_FOREACH ( i, dTagStrings )
		dTags[i] = sphFNV64 ( dTagStrings[i].cstr() );
	dTags.Uniq();
}

static bool TagsMatched ( const VecTraits_T<uint64_t>& dFilter, const VecTraits_T<uint64_t>& dQueryTags, bool bTagsEq=true )
{
	auto pFilter = dFilter.begin();
	auto pQueryTags = dQueryTags.begin();
	auto pFilterEnd = dFilter.end();
	auto pTagsEnd = dQueryTags.end();

	while ( pFilter<pFilterEnd && pQueryTags<pTagsEnd )
	{
		if ( *pQueryTags<*pFilter )
			++pQueryTags;
		else if ( *pFilter<*pQueryTags )
			++pFilter;
		else if ( *pQueryTags==*pFilter )
			return bTagsEq;
	}
	return !bTagsEq;
}

//////////////////////////////////////////////////////////////////////////
// percolate index definition

PercolateIndex_c::PercolateIndex_c ( const CSphSchema & tSchema, const char * sIndexName, const char * sPath )
	: PercolateIndex_i ( sIndexName, sPath )
{
	m_tSchema = tSchema;

	// fill match schema
	m_tMatchSchema.AddAttr ( CSphColumnInfo ( "uid", SPH_ATTR_BIGINT ), true );
	m_tMatchSchema.AddAttr ( CSphColumnInfo ( "query", SPH_ATTR_STRINGPTR ), true );
	m_tMatchSchema.AddAttr ( CSphColumnInfo ( "tags", SPH_ATTR_STRINGPTR ), true );
	m_tMatchSchema.AddAttr ( CSphColumnInfo ( "filters", SPH_ATTR_STRINGPTR ), true );
}

PercolateIndex_c::~PercolateIndex_c ()
{
	bool bValid = m_pTokenizer && m_pDict;
	if ( bValid )
		SaveMeta();

	{ // coverity complains about accessing m_dStored without locking tLock
		ScWL_t wLock { m_tLock };
		for ( auto& dStored : m_dStored )
			SafeDelete ( dStored.m_pQuery );
	}
	SafeClose ( m_iLockFD );
}

ISphRtAccum * PercolateIndex_c::CreateAccum ( CSphString & sError )
{
	return AcquireAccum ( m_pDict, nullptr, true, false, &sError );
}


bool PercolateIndex_c::AddDocument ( ISphTokenizer * pTokenizer, int iFields, const char ** ppFields, const CSphMatch & tDoc, bool , const CSphString & , const char ** ppStr, const CSphVector<DWORD> & dMvas, CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt )
{
	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, true, true, &sError );
	if ( !pAcc )
		return false;

	// FIXME!!! move setup to preparation or CloneIndexingTokenizer
	if ( m_tSettings.m_uAotFilterMask )
		pTokenizer = sphAotCreateFilter ( pTokenizer, m_pDict, m_tSettings.m_bIndexExactWords, m_tSettings.m_uAotFilterMask );

	ISphTokenizerRefPtr_c tTokenizer { pTokenizer };

	// SPZ setup
	if ( m_tSettings.m_bIndexSP && !pTokenizer->EnableSentenceIndexing ( sError ) )
		return false;

	if ( !m_tSettings.m_sZones.IsEmpty() && !pTokenizer->EnableZoneIndexing ( sError ) )
		return false;

	CSphSource_StringVector tSrc ( iFields, ppFields, m_tSchema );
	if ( m_tSettings.m_bHtmlStrip &&
		!tSrc.SetStripHTML ( m_tSettings.m_sHtmlIndexAttrs.cstr(), m_tSettings.m_sHtmlRemoveElements.cstr(),
			m_tSettings.m_bIndexSP, m_tSettings.m_sZones.cstr(), sError ) )
		return false;

	ISphFieldFilterRefPtr_c pFieldFilter;
	if ( m_pFieldFilter )
		pFieldFilter = m_pFieldFilter->Clone();

	// TODO: field filter \ token filter?
	tSrc.Setup ( m_tSettings );
	tSrc.SetTokenizer ( pTokenizer );
	tSrc.SetDict ( pAcc->m_pDict );
	tSrc.SetFieldFilter ( pFieldFilter );
	if ( !tSrc.Connect ( m_sLastError ) )
		return false;

	m_tSchema.CloneWholeMatch ( &tSrc.m_tDocInfo, tDoc );

	if ( !tSrc.IterateStart ( sError ) || !tSrc.IterateDocument ( sError ) )
		return false;

	ISphHits * pHits = tSrc.IterateHits ( sError );
	pAcc->GrabLastWarning ( sWarning );

	pAcc->AddDocument ( pHits, tDoc, true, m_tSchema.GetRowSize(), ppStr, dMvas );

	return true;
}

//////////////////////////////////////////////////////////////////////////
// percolate Qword

struct PercolateQword_t : public ISphQword
{
public:
	PercolateQword_t () = default;
	virtual ~PercolateQword_t ()
	{
	}

	const CSphMatch & GetNextDoc ( DWORD * ) final
	{
		m_iHits = 0;
		while (true)
		{
			const RtDoc_t * pDoc = m_tDocReader.UnzipDoc();
			if ( !pDoc && m_iDoc>=m_dDoclist.GetLength() )
			{
				m_tMatch.m_uDocID = 0;
				return m_tMatch;
			}

			if ( !pDoc )
			{
				SetupReader();
				pDoc = m_tDocReader.UnzipDoc();
				assert ( pDoc );
			}

			m_tMatch.m_uDocID = pDoc->m_uDocID;
			m_dQwordFields.Assign32 ( pDoc->m_uDocFields );
			m_uMatchHits = pDoc->m_uHits;
			m_iHitlistPos = (uint64_t(pDoc->m_uHits)<<32) + pDoc->m_uHit;
			m_bAllFieldsKnown = false;

			return m_tMatch;
		}
	}

	void SeekHitlist ( SphOffset_t uOff ) final
	{
		int iHits = (int)(uOff>>32);
		if ( iHits==1 )
		{
			m_uNextHit = DWORD(uOff);
		} else
		{
			m_uNextHit = 0;
			m_tHitReader.Seek ( DWORD(uOff), iHits );
		}
	}

	Hitpos_t GetNextHit () final
	{
		if ( m_uNextHit==0 )
			return Hitpos_t ( m_tHitReader.UnzipHit() );
		else if ( m_uNextHit==0xffffffffUL )
			return EMPTY_HIT;
		else
		{
			Hitpos_t tHit ( m_uNextHit );
			m_uNextHit = 0xffffffffUL;
			return tHit;
		}
	}

	bool Setup ( const RtSegment_t * pSeg, CSphVector<Slice_t> & dDoclist )
	{
		m_iDoc = 0;
		m_tDocReader = RtDocReader_t();
		m_pSeg = pSeg;
		m_tHitReader.m_pBase = pSeg->m_dHits.Begin();

		m_dDoclist.Set ( dDoclist.Begin(), dDoclist.GetLength() );
		dDoclist.LeakData();

		if ( m_iDoc && m_iDoc>=m_dDoclist.GetLength() )
			return false;

		SetupReader();
		return true;
	}

private:
	void SetupReader ()
	{
		RtWord_t tWord;
		tWord.m_uDoc = m_dDoclist[m_iDoc].m_uOff;
		tWord.m_uDocs = m_dDoclist[m_iDoc].m_uLen;
		m_tDocReader = RtDocReader_t ( m_pSeg, tWord );
		m_iDoc++;
	}

	const RtSegment_t *			m_pSeg = nullptr;
	CSphFixedVector<Slice_t>	m_dDoclist { 0 };
	CSphMatch					m_tMatch;
	RtDocReader_t				m_tDocReader;
	RtHitReader2_t				m_tHitReader;

	int							m_iDoc = 0;
	DWORD						m_uNextHit = 0;
};


enum class PERCOLATE
{
	EXACT,
	PREFIX,
	INFIX
};

class PercolateQwordSetup_c : public ISphQwordSetup
{
public:
	PercolateQwordSetup_c ( const RtSegment_t * pSeg, int iMaxCodepointLength )
		: m_pSeg ( pSeg )
		, m_iMaxCodepointLength ( iMaxCodepointLength )
	{}

	ISphQword *	QwordSpawn ( const XQKeyword_t & ) const final;
	bool		QwordSetup ( ISphQword * pQword ) const final;

private:
	const RtSegment_t * m_pSeg;
	int					m_iMaxCodepointLength;
};

ISphQword *	PercolateQwordSetup_c::QwordSpawn ( const XQKeyword_t & ) const
{
	return new PercolateQword_t();
}

bool PercolateQwordSetup_c::QwordSetup ( ISphQword * pQword ) const
{
	auto * pMyQword = (PercolateQword_t *)pQword;
	const char * sWord = pMyQword->m_sDictWord.cstr();
	int iWordLen = pMyQword->m_sDictWord.Length();
	if ( !iWordLen )
		return false;

	SubstringInfo_t tSubInfo;
	CSphVector<Slice_t> dDictLoc;
	PERCOLATE eCmp = PERCOLATE::EXACT;
	if ( !sphHasExpandableWildcards ( sWord ) )
	{
		// no wild-cards, or just wild-cards? do not expand
		Slice_t tChPoint = GetTermLocator ( sWord, iWordLen, m_pSeg );
		dDictLoc.Add ( tChPoint );
	} else if ( !sphIsWild ( *sWord ) )
	{
		eCmp = PERCOLATE::PREFIX;
		Slice_t tChPoint = GetPrefixLocator ( sWord, m_pDict->HasMorphology(), m_pSeg, tSubInfo );
		dDictLoc.Add ( tChPoint );
	} else
	{
		eCmp = PERCOLATE::INFIX;
		GetSuffixLocators ( sWord, m_iMaxCodepointLength, m_pSeg, tSubInfo, dDictLoc );
	}

	// to skip heading magic chars ( NONSTEMMED ) in the prefix
	int iSkipMagic = 0;
	if ( eCmp==PERCOLATE::PREFIX || eCmp==PERCOLATE::INFIX )
		iSkipMagic = ( BYTE ( *tSubInfo.m_sSubstring )<0x20 );

	// cases:
	// empty - check all words
	// no matches - check only words prior to 1st checkpoint
	// checkpoint found - check words at that checkpoint
	const BYTE * pWordBase = m_pSeg->m_dWords.Begin();
	CSphVector<Slice_t> dDictWords;
	ARRAY_FOREACH ( i, dDictLoc )
	{
		RtWordReader_t tReader ( m_pSeg, true, PERCOLATE_WORDS_PER_CP );
		// locator
		// m_uOff - Start
		// m_uLen - End
		tReader.m_pCur = pWordBase + dDictLoc[i].m_uOff;
		tReader.m_pMax = pWordBase + dDictLoc[i].m_uLen;

		const RtWord_t * pWord = NULL;
		while ( ( pWord = tReader.UnzipWord() )!=NULL )
		{
			// stemmed terms do not match any kind of wild-cards
			if ( ( eCmp==PERCOLATE::PREFIX || eCmp==PERCOLATE::INFIX ) && m_pDict->HasMorphology() && pWord->m_sWord[1]!=MAGIC_WORD_HEAD_NONSTEMMED )
				continue;

			int iCmp = -1;
			switch ( eCmp )
			{
			case PERCOLATE::EXACT:
				iCmp = sphDictCmpStrictly ( (const char *)pWord->m_sWord + 1, pWord->m_sWord[0], sWord, iWordLen );
				break;

			case PERCOLATE::PREFIX:
				iCmp = sphDictCmp ( (const char *)pWord->m_sWord + 1, pWord->m_sWord[0], tSubInfo.m_sSubstring, tSubInfo.m_iSubLen );
				if ( iCmp==0 )
				{
					if ( !( tSubInfo.m_iSubLen<=pWord->m_sWord[0] && sphWildcardMatch ( (const char *)pWord->m_sWord + 1 + iSkipMagic, tSubInfo.m_sWildcard ) ) )
						iCmp = -1;
				}
				break;

			case PERCOLATE::INFIX:
				if ( sphWildcardMatch ( (const char *)pWord->m_sWord + 1 + iSkipMagic, tSubInfo.m_sWildcard ) )
					iCmp = 0;
				break;

			default: break;
			}

			if ( iCmp==0 )
			{
				pMyQword->m_iDocs += pWord->m_uDocs;
				pMyQword->m_iHits += pWord->m_uHits;

				Slice_t & tDictPoint = dDictWords.Add();
				tDictPoint.m_uOff = pWord->m_uDoc;
				tDictPoint.m_uLen = pWord->m_uDocs;
			}

			if ( iCmp>0 || ( iCmp==0 && eCmp==PERCOLATE::EXACT ) )
				break;
		}
	}

	bool bWordSet = false;
	if ( dDictWords.GetLength() )
	{
		dDictWords.Sort ( bind ( &Slice_t::m_uOff ) );
		bWordSet = pMyQword->Setup ( m_pSeg, dDictWords );
	}

	return bWordSet;
}


SphWordID_t DictMap_t::GetTerm ( BYTE * sWord ) const
{
	const DictTerm_t * pTerm = m_hTerms.Find ( sphFNV64 ( sWord ) );
	if ( !pTerm )
		return 0;

	memcpy ( sWord, m_dKeywords.Begin() + pTerm->m_iWordOff, pTerm->m_iWordLen );
	return pTerm->m_uWordID;
}

class PercolateDictProxy_c : public CSphDict
{
	const DictMap_t * m_pDict = NULL;
	const bool m_bHasMorph = false;

public:
	explicit PercolateDictProxy_c ( bool bHasMorph )
		: m_bHasMorph ( bHasMorph )
	{
	}

	void SetMap ( const DictMap_t & hDict )
	{
		m_pDict = &hDict;
	}

	// these only got called actually
	SphWordID_t	GetWordID ( BYTE * pWord ) override
	{
		assert ( m_pDict );
		return const_cast<DictMap_t *>(m_pDict)->GetTerm ( pWord );
	}

	SphWordID_t	GetWordIDNonStemmed ( BYTE * pWord ) override
	{
		assert ( m_pDict );
		return const_cast<DictMap_t *>(m_pDict)->GetTerm ( pWord );
	}

	bool		HasMorphology () const override { return m_bHasMorph; }

	// not implemented
	CSphDictSettings m_tDummySettings;
	CSphVector <CSphSavedFile> m_tDummySF;

	SphWordID_t GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops ) override { return 0; }
	void LoadStopwords ( const CSphVector<SphWordID_t> & dStopwords ) override {}
	void LoadStopwords ( const char * sFiles, const ISphTokenizer * pTokenizer ) override {}
	void WriteStopwords ( CSphWriter & tWriter ) const override {}
	bool LoadWordforms ( const StrVec_t &, const CSphEmbeddedFiles * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex ) override { return false; }
	void WriteWordforms ( CSphWriter & tWriter ) const override {}
	int SetMorphology ( const char * szMorph, CSphString & sMessage ) override { return 0; }
	void Setup ( const CSphDictSettings & tSettings )  override {}
	const CSphDictSettings & GetSettings () const  override { return m_tDummySettings; }
	const CSphVector <CSphSavedFile> & GetStopwordsFileInfos () const override { return m_tDummySF; }
	const CSphVector <CSphSavedFile> & GetWordformsFileInfos () const override { return m_tDummySF; }
	const CSphMultiformContainer * GetMultiWordforms () const override { return nullptr; }
	bool IsStopWord ( const BYTE * pWord ) const  override { return false; }
	uint64_t GetSettingsFNV () const override { return 0; }
};

struct PercolateMatchContext_t
{
	CSphVector<PercolateQueryDesc> m_dQueryMatched;
	CSphVector<int> m_dDocsMatched;
	CSphVector<int> m_dDt;
	int m_iQueriesMatched = 0;
	int m_iDocsMatched = 0;
	int m_iEarlyPassed = 0;
	int m_iOnlyTerms = 0;
	bool m_bGetDocs = false;
	bool m_bGetQuery = false;
	bool m_bGetFilters = false;
	bool m_bVerbose = false;
	int m_iQueriesFailed = 0;

	StringBuilder_c m_tFilterBuf;
	KillListVector m_dKillist;

	PercolateDictProxy_c m_tDictMap;
	CSphQuery m_tDummyQuery;
	CSphQueryContext * m_pCtx = nullptr;
	PercolateQwordSetup_c * m_pTermSetup = nullptr;

	// const actually shared between all workers
	const ISphSchema & m_tSchema;
	const SegmentReject_t & m_tReject;
	const bool m_bUtf8 = false;

	PercolateMatchContext_t ( const RtSegment_t * pSeg, int iMaxCodepointLength, bool bHasMorph, const PercolateIndex_c * pIndex,
		const ISphSchema & tSchema, const SegmentReject_t & tReject )
		: m_tDictMap ( bHasMorph )
		, m_tSchema ( tSchema )
		, m_tReject ( tReject )
		, m_bUtf8 ( iMaxCodepointLength>1 )
	{
		m_tDummyQuery.m_eRanker = SPH_RANK_NONE;
		m_pCtx = new CSphQueryContext ( m_tDummyQuery );
		m_pCtx->m_bSkipQCache = true;
		// for lookups to work
		m_pCtx->m_pIndexData = pSeg;

		// setup search terms
		m_pTermSetup = new PercolateQwordSetup_c ( pSeg, iMaxCodepointLength );
		m_pTermSetup->SetDict ( &m_tDictMap );
		m_pTermSetup->m_pIndex = pIndex;
		m_pTermSetup->m_pCtx = m_pCtx;
	};

	~PercolateMatchContext_t ()
	{
		SafeDelete ( m_pTermSetup );
		SafeDelete ( m_pCtx );
	}
};

// percolate matching
static void MatchingWork ( const StoredQuery_t * pStored, PercolateMatchContext_t & tMatchCtx )
{
	uint64_t tmQueryStart = ( tMatchCtx.m_bVerbose ? sphMicroTimer() : 0 );
	tMatchCtx.m_iOnlyTerms += ( pStored->m_bOnlyTerms ? 1 : 0 );

	if ( !pStored->IsFullscan() && tMatchCtx.m_tReject.Filter ( pStored, tMatchCtx.m_bUtf8 ) )
		return;

	const RtSegment_t * pSeg = (RtSegment_t *)tMatchCtx.m_pCtx->m_pIndexData;
	const BYTE * pStrings = pSeg->m_dStrings.Begin();
	const DWORD * pMva = pSeg->m_dMvas.Begin();

	tMatchCtx.m_iEarlyPassed++;
	tMatchCtx.m_pCtx->ResetFilters();

	// FIXME!!! collect and show all errors and warnings somehow
	CSphString sError;
	CSphString sWarning;

	// setup filters
	CreateFilterContext_t tFlx;
	tFlx.m_pFilters = &pStored->m_dFilters;
	tFlx.m_pFilterTree = &pStored->m_dFilterTree;
	tFlx.m_pKillList = &tMatchCtx.m_dKillist;
	tFlx.m_pSchema = &tMatchCtx.m_tSchema;
	tFlx.m_pMvaPool = pMva;
	tFlx.m_pStrings = pStrings;
	tFlx.m_eCollation = SPH_COLLATION_DEFAULT;
	tFlx.m_bArenaProhibit = true;

	if ( !tMatchCtx.m_pCtx->CreateFilters ( tFlx, sError, sWarning ) )
	{
		++tMatchCtx.m_iQueriesFailed;
		return;
	}


	const bool bCollectDocs = tMatchCtx.m_bGetDocs;
	int iDocsOff = tMatchCtx.m_dDocsMatched.GetLength();
	int iMatchCount = 0;
	// reserve space for matched docs counter
	if ( bCollectDocs )
		tMatchCtx.m_dDocsMatched.Add ( 0 );


	if ( !pStored->IsFullscan() ) // matching path
	{
		// set terms dictionary
		tMatchCtx.m_tDictMap.SetMap ( pStored->m_hDict );
		CSphQueryResult tTmpResult;
		CSphScopedPtr<ISphRanker> pRanker ( sphCreateRanker ( *pStored->m_pXQ, &tMatchCtx.m_tDummyQuery, &tTmpResult, *tMatchCtx.m_pTermSetup, *tMatchCtx.m_pCtx, tMatchCtx.m_tSchema ) );

		if ( !pRanker.Ptr() )
			return;

		const CSphMatch * pMatch = pRanker->GetMatchesBuffer();
		while ( true )
		{
			int iMatches = pRanker->GetMatches();
			if ( !iMatches )
				break;

			if ( bCollectDocs )
			{
				// docs encoding: docs-count; docs matched
				tMatchCtx.m_dDocsMatched.Reserve ( tMatchCtx.m_dDocsMatched.GetLength() + iMatches );
				for ( int iMatch=0; iMatch<iMatches; iMatch++ )
					tMatchCtx.m_dDocsMatched.Add ( pMatch[iMatch].m_uDocID );
			}

			iMatchCount += iMatches;
		}
	} else // full-scan path
	{
		CSphMatch tDoc;
		int iStride = DOCINFO_IDSIZE + tMatchCtx.m_tSchema.GetRowSize();
		const CSphIndex * pIndex = tMatchCtx.m_pTermSetup->m_pIndex;
		const CSphRowitem * pRow = pSeg->m_dRows.Begin();
		for ( int i = 0; i<pSeg->m_iRows; i++ )
		{
			tDoc.m_uDocID = DOCINFO2ID ( pRow );
			pRow += iStride;
			if ( pIndex->EarlyReject ( tMatchCtx.m_pCtx, tDoc ) )
				continue;

			iMatchCount++;
			if ( bCollectDocs ) // keep matched docs
				tMatchCtx.m_dDocsMatched.Add ( tDoc.m_uDocID );
		}
	}

	if ( iMatchCount )
	{
		tMatchCtx.m_iQueriesMatched++;
		tMatchCtx.m_iDocsMatched += iMatchCount;

		PercolateQueryDesc & tDesc = tMatchCtx.m_dQueryMatched.Add();
		tDesc.m_uID = pStored->m_uUID;
		if ( bCollectDocs )
			tMatchCtx.m_dDocsMatched[iDocsOff] = iMatchCount;
		if ( tMatchCtx.m_bGetQuery )
		{
			tDesc.m_sQuery = pStored->m_sQuery;
			tDesc.m_sTags = pStored->m_sTags;
			tDesc.m_bQL = pStored->m_bQL;

			if ( tMatchCtx.m_bGetFilters && pStored->m_dFilters.GetLength() )
			{
				tMatchCtx.m_tFilterBuf.Clear();
				FormatFiltersQL ( pStored->m_dFilters, pStored->m_dFilterTree, tMatchCtx.m_tFilterBuf );
				tDesc.m_sFilters = tMatchCtx.m_tFilterBuf.cstr();
			}
		}

	if ( tMatchCtx.m_bVerbose )
		tMatchCtx.m_dDt.Add ( (int)( sphMicroTimer() - tmQueryStart ) );

	} else if ( bCollectDocs ) // pop's up reserved but not used matched counter
	{
		tMatchCtx.m_dDocsMatched.Resize ( iDocsOff );
	}
}


struct PercolateMatchJob_t : public ISphJob
{
	const CSphVector<StoredQueryKey_t> & m_dStored;
	CSphAtomic & m_tQueryCounter;
	PercolateMatchContext_t & m_tMatchCtx;
	
	const CrashQuery_t * m_pCrashQuery = nullptr;

	PercolateMatchJob_t ( const CSphVector<StoredQueryKey_t> & dStored, CSphAtomic & tQueryCounter, PercolateMatchContext_t & tMatchCtx, const CrashQuery_t * pCrashQuery )
		: m_dStored ( dStored )
		, m_tQueryCounter ( tQueryCounter )
		, m_tMatchCtx ( tMatchCtx )
		, m_pCrashQuery ( pCrashQuery )
	{}

	virtual ~PercolateMatchJob_t () override
	{}

	virtual void Call () override
	{
		CrashQuery_t tQueryTLS;
		if ( m_pCrashQuery )
		{
			CrashQuerySetTop ( &tQueryTLS ); // set crash info container
			CrashQuerySet ( *m_pCrashQuery ); // transfer crash info into container
		}

		while ( true )
		{
			int iQuery = m_tQueryCounter.Inc();
			if ( iQuery>=m_dStored.GetLength() )
				break;

			MatchingWork ( m_dStored[iQuery].m_pQuery, m_tMatchCtx );
		}
	}
};

void PercolateQueryDesc::Swap ( PercolateQueryDesc & tOther )
{
	::Swap ( m_uID, tOther.m_uID );
	::Swap ( m_bQL, tOther.m_bQL );

	m_sQuery.Swap ( tOther.m_sQuery );
	m_sTags.Swap ( tOther.m_sTags );
	m_sFilters.Swap ( tOther.m_sFilters );
}

struct PercolateMergeIterator_t
{
	PercolateQueryDesc * m_pCur = nullptr;
	const PercolateQueryDesc * m_pEnd = nullptr;
	int m_iCtx = 0;
	int m_iDocOff = 0;
};

static void PercolateGetResult ( int iTotalQueries, CSphFixedVector<PercolateMatchContext_t *> & dMatches, PercolateMatchResult_t & tRes )
{
	if ( !dMatches.GetLength() )
		return;

	// fast path for just swapping result set
	if ( dMatches.GetLength()==1 )
	{
		PercolateMatchContext_t * pMatch = dMatches[0];

		tRes.m_iQueriesMatched = pMatch->m_iQueriesMatched;
		tRes.m_iDocsMatched = pMatch->m_iDocsMatched;
		tRes.m_iTotalQueries = iTotalQueries;
		tRes.m_iEarlyOutQueries = ( iTotalQueries - pMatch->m_iEarlyPassed );
		tRes.m_iOnlyTerms = pMatch->m_iOnlyTerms;
		tRes.m_iQueriesFailed = pMatch->m_iQueriesFailed;
		if ( tRes.m_bVerbose )
			tRes.m_dQueryDT.CopyFrom ( pMatch->m_dDt );

		// result set
		tRes.m_dQueryDesc.Reset ( pMatch->m_dQueryMatched.GetLength() );
		ARRAY_FOREACH ( i, pMatch->m_dQueryMatched )
			tRes.m_dQueryDesc[i].Swap ( pMatch->m_dQueryMatched[i] );

		if ( tRes.m_bGetDocs )
			tRes.m_dDocs.CopyFrom ( pMatch->m_dDocsMatched );

		return;
	}

	int iGotQueries = 0;
	int iGotDocs = 0;
	CSphVector<PercolateMergeIterator_t> dIters ( dMatches.GetLength() );
	dIters.Resize ( 0 );
	ARRAY_FOREACH ( i, dMatches )
	{
		PercolateMatchContext_t * pMatch = dMatches[i];
		if ( !pMatch->m_dQueryMatched.GetLength() )
			continue;

		PercolateMergeIterator_t & tIt = dIters.Add();
		tIt.m_iCtx = i;
		tIt.m_pCur = pMatch->m_dQueryMatched.Begin();
		tIt.m_pEnd = pMatch->m_dQueryMatched.Begin() + pMatch->m_dQueryMatched.GetLength();

		iGotQueries += pMatch->m_dQueryMatched.GetLength();
		iGotDocs += pMatch->m_dDocsMatched.GetLength();

		tRes.m_iQueriesMatched += pMatch->m_iQueriesMatched;
		tRes.m_iDocsMatched += pMatch->m_iDocsMatched;
		tRes.m_iEarlyOutQueries += pMatch->m_iEarlyPassed;
		tRes.m_iOnlyTerms += pMatch->m_iOnlyTerms;
		tRes.m_iQueriesFailed += pMatch->m_iQueriesFailed;
	}
	tRes.m_iTotalQueries = iTotalQueries;
	tRes.m_iEarlyOutQueries = ( iTotalQueries - tRes.m_iEarlyOutQueries );

	tRes.m_dQueryDesc.Reset ( iGotQueries );
	tRes.m_dDocs.Reset ( iGotDocs );
	PercolateQueryDesc * pDst = tRes.m_dQueryDesc.Begin();

	int iDstDocOffset = 0;
	while ( dIters.GetLength() )
	{
		int iMinIt = 0;
		for ( int i=1; i<dIters.GetLength(); i++ )
		{
			if ( dIters[i].m_pCur->m_uID<dIters[iMinIt].m_pCur->m_uID )
				iMinIt = i;
		}

		PercolateMergeIterator_t & tMinIt = dIters[iMinIt];
		pDst->Swap ( *tMinIt.m_pCur );

		// docs copy
		if ( tRes.m_bGetDocs )
		{
			int iDocOff = tMinIt.m_iDocOff;
			int iDocCount = dMatches[tMinIt.m_iCtx]->m_dDocsMatched[iDocOff];

			memcpy ( tRes.m_dDocs.Begin() + iDstDocOffset, dMatches[tMinIt.m_iCtx]->m_dDocsMatched.Begin() + iDocOff, sizeof(tRes.m_dDocs[0]) * ( iDocCount + 1 ) );

			tMinIt.m_iDocOff += iDocCount + 1;
			iDstDocOffset += iDocCount + 1;
		}

		// iterate next and remove on read all data
		pDst++;
		tMinIt.m_pCur++;
		if ( tMinIt.m_pCur==tMinIt.m_pEnd )
			dIters.RemoveFast ( iMinIt );
	}
}

void PercolateIndex_c::DoMatchDocuments ( const RtSegment_t * pSeg, PercolateMatchResult_t & tRes )
{
	SegmentReject_t tReject;
	// reject need bloom filter for either infix or prefix
	SegmentGetRejects ( pSeg, ( m_tSettings.m_iMinInfixLen>0 || m_tSettings.m_iMinPrefixLen>0 ),
		( m_iMaxCodepointLength>1 ), tReject );

	CSphAtomic tQueryCounter ( 0 );
	CSphFixedVector<PercolateMatchContext_t *> dMatches ( 1 );
	ISphThdPool * pPool = nullptr;
	CrashQuery_t tCrashQuery;

	// pool jobs only for decent amount of queries
	if ( g_iPercolateThreads>1 && m_dStored.GetLength()>4 )
	{
		int iThreads = Min ( g_iPercolateThreads, m_dStored.GetLength() );
		dMatches.Reset ( iThreads );
		// one job always goes at current thread
		CSphString sError;
		pPool = sphThreadPoolCreate ( iThreads-1, "percolate", sError );
		if ( !pPool )
			sphWarning( "failed to create thread_pool, single thread matching used: %s", sError.cstr() );
	}

	ARRAY_FOREACH ( i, dMatches )
	{
		PercolateMatchContext_t * pMatchCtx = new PercolateMatchContext_t ( pSeg, m_iMaxCodepointLength, m_pDict->HasMorphology(), this, m_tSchema, tReject );
		pMatchCtx->m_bGetDocs = tRes.m_bGetDocs;
		pMatchCtx->m_bGetQuery = tRes.m_bGetQuery;
		pMatchCtx->m_bGetFilters = tRes.m_bGetFilters;
		pMatchCtx->m_bVerbose = tRes.m_bVerbose;
		dMatches[i] = pMatchCtx;
	}

	if ( tRes.m_bVerbose )
		tRes.m_tmSetup = sphMicroTimer() - tRes.m_tmSetup;

	// queries should be locked for reading now
	int iTotalQueries = 0;
	{
		ScRL_t rLock ( m_tLock );

		iTotalQueries = m_dStored.GetLength();
		PercolateMatchJob_t tJobMain ( m_dStored, tQueryCounter, *dMatches[0], nullptr ); // still got crash info no need to set it again

		// work loop
		if ( pPool )
		{
			tCrashQuery = CrashQueryGet();
			for ( int i=1; i<dMatches.GetLength(); i++ )
			{
				PercolateMatchJob_t * pJob = new PercolateMatchJob_t ( m_dStored, tQueryCounter, *dMatches[i], &tCrashQuery );
				pPool->AddJob ( pJob );
			}
		}
		tJobMain.Call();
		if ( pPool )
			pPool->Shutdown();
		SafeDelete ( pPool );

	}

	// merge result set
	PercolateGetResult ( iTotalQueries, dMatches, tRes );

	for ( auto & dMatch: dMatches )
		SafeDelete ( dMatch );
}


bool PercolateIndex_c::MatchDocuments ( ISphRtAccum * pAccExt, PercolateMatchResult_t & tRes )
{
	MEMORY ( MEM_INDEX_RT );

	int64_t tmStart = sphMicroTimer();
	tRes.m_tmSetup = tmStart;
	m_sLastWarning = "";

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt );
	if ( !pAcc )
		return false;

	// empty txn or no queries just ignore
	{
		ScRL_t rLock ( m_tLock );
		if ( !pAcc->m_iAccumDocs || m_dStored.IsEmpty() )
		{
			pAcc->Cleanup ();
			return true;
		}
	}

	pAcc->Sort();

	RtSegment_t * pSeg = pAcc->CreateSegment ( m_tSchema.GetRowSize(), PERCOLATE_WORDS_PER_CP );
	assert ( !pSeg || pSeg->m_iRows>0 );
	assert ( !pSeg || pSeg->m_iAliveRows>0 );
	assert ( !pSeg || !pSeg->m_bTlsKlist );
	BuildSegmentInfixes ( pSeg, m_pDict->HasMorphology(), true, m_tSettings.m_iMinInfixLen, PERCOLATE_WORDS_PER_CP, ( m_iMaxCodepointLength>1 ) );

	DoMatchDocuments ( pSeg, tRes );
	SafeDelete ( pSeg );

	// done; cleanup accum
	pAcc->Cleanup ();

	int64_t tmEnd = sphMicroTimer();
	tRes.m_tmTotal = tmEnd - tmStart;
	return true;
}

void PercolateIndex_c::RollBack ( ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt );
	if ( !pAcc )
		return;

	pAcc->Cleanup ();
}

bool PercolateIndex_c::EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const
{
	if ( !pCtx->m_pFilter )
		return false;

	int iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	const CSphRowitem * pRow = FindDocinfo ( (RtSegment_t*)pCtx->m_pIndexData, tMatch.m_uDocID, iStride );
	if ( !pRow )
		return true;
	CopyDocinfo ( tMatch, pRow );

	return !pCtx->m_pFilter->Eval ( tMatch );
}


bool PercolateIndex_c::Query ( const char * sQuery, const char * sTags, const CSphVector<CSphFilterSettings> * pFilters,
	const CSphVector<FilterTreeItem_t> * pFilterTree, bool bReplace, bool bQL, uint64_t & uId, CSphString & sError )
{
	ISphTokenizerRefPtr_c pTokenizer ( m_pTokenizer->Clone ( SPH_CLONE_QUERY ) );
	sphSetupQueryTokenizer ( pTokenizer, IsStarDict(), m_tSettings.m_bIndexExactWords, false );

	CSphDictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( IsStarDict () )
		SetupStarDict ( pDict, pTokenizer );

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pTokenizer );

	const ISphTokenizer * pTok = pTokenizer;
	ISphTokenizerRefPtr_c pTokenizerJson;
	if ( !bQL )
	{
		pTokenizerJson = m_pTokenizer->Clone ( SPH_CLONE_QUERY );
		sphSetupQueryTokenizer ( pTokenizerJson, IsStarDict (), m_tSettings.m_bIndexExactWords, true );
		pTok = pTokenizerJson;
	}
	return AddQuery ( sQuery, sTags, pFilters, pFilterTree, bReplace, bQL, uId, pTok, pDict, sError );
}


static const QueryParser_i * CreatePlainQueryparser ( bool )
{
	return sphCreatePlainQueryParser();
}

static CreateQueryParser * g_pCreateQueryParser = CreatePlainQueryparser;
void SetPercolateQueryParserFactory ( CreateQueryParser * pCall )
{
	g_pCreateQueryParser = pCall;
}

static void FixExpanded ( XQNode_t * pNode )
{
	assert ( pNode );

	ARRAY_FOREACH ( i, pNode->m_dWords )
	{
		XQKeyword_t & tKw = pNode->m_dWords[i];
		if ( sphHasExpandableWildcards ( tKw.m_sWord.cstr() ) )
		{
			tKw.m_bExpanded = true;
			// that pointer has not owned by XQKeyword_t and will NOT be deleted
			// however it should be !=nullptr to create ExtPayload_c at ranker
			tKw.m_pPayload = (void *)1;
		}
	}

	ARRAY_FOREACH ( i, pNode->m_dChildren )
		FixExpanded ( pNode->m_dChildren[i] );
}

bool PercolateIndex_c::AddQuery ( const char * sQuery, const char * sTags, const CSphVector<CSphFilterSettings> * pFilters,
	const CSphVector<FilterTreeItem_t> * pFilterTree, bool bReplace, bool bQL, uint64_t & uId,
	const ISphTokenizer * pTokenizer, CSphDict * pDict, CSphString & sError )
{
	CSphVector<BYTE> dFiltered;
	if ( m_pFieldFilter && sQuery )
	{
		ISphFieldFilterRefPtr_c pFieldFilter { m_pFieldFilter->Clone() };
		if ( pFieldFilter && pFieldFilter->Apply ( (const BYTE *)sQuery, strlen ( sQuery ), dFiltered, true ) )
			sQuery = (const char *)dFiltered.Begin();
	}

	CSphScopedPtr<XQQuery_t> tParsed ( new XQQuery_t() );
	CSphScopedPtr<const QueryParser_i> tParser ( g_pCreateQueryParser ( !bQL ) );

	// right tokenizer created at upper level
	bool bParsed = tParser->ParseQuery ( *tParsed.Ptr(), sQuery, nullptr, pTokenizer, pTokenizer, &m_tSchema, pDict, m_tSettings );
	if ( !bParsed )
	{
		sError = tParsed->m_sParseError;
		return false;
	}

	// FIXME!!! provide segments list instead index
	sphTransformExtendedQuery ( &tParsed->m_pRoot, m_tSettings, false, NULL );

	if ( m_iExpandKeywords!=KWE_DISABLED )
	{
		tParsed->m_pRoot = sphQueryExpandKeywords ( tParsed->m_pRoot, m_tSettings, m_iExpandKeywords );
		tParsed->m_pRoot->Check ( true );
	}

	// this should be after keyword expansion
	if ( m_tSettings.m_uAotFilterMask )
		TransformAotFilter ( tParsed->m_pRoot, pDict->GetWordforms(), m_tSettings );

	if ( m_tSettings.m_iMinPrefixLen>0 || m_tSettings.m_iMinInfixLen>0 )
		FixExpanded ( tParsed->m_pRoot );

	auto pStored = new StoredQuery_t();
	pStored->m_pXQ = tParsed.LeakPtr();
	pStored->m_bOnlyTerms = true;
	pStored->m_sQuery = sQuery;
	QueryGetRejects ( pStored->m_pXQ->m_pRoot, pDict, pStored->m_dRejectTerms, pStored->m_dRejectWilds, pStored->m_dSuffixes, pStored->m_bOnlyTerms, ( m_iMaxCodepointLength>1 ) );
	QueryGetTerms ( pStored->m_pXQ->m_pRoot, pDict, pStored->m_hDict );
	pStored->m_sTags = sTags;
	PercolateTags ( sTags, pStored->m_dTags );
	pStored->m_uUID = uId;
	if ( pFilters && pFilters->GetLength() )
		pStored->m_dFilters = *pFilters;
	if ( pFilterTree && pFilterTree->GetLength() )
		pStored->m_dFilterTree = *pFilterTree;
	pStored->m_bQL = bQL;

	ScWL_t wLock (m_tLock);

	bool bAutoID = ( uId==0 );
	if ( bAutoID )
		uId = ( m_dStored.GetLength() ? m_dStored.Last().m_uUID + 1 : 1 );

	StoredQueryKey_t tItem { uId, pStored };
	pStored->m_uUID = uId;

	bool bAdded = true;
	if ( bAutoID )
	{
		m_dStored.Add ( tItem );
	} else
	{
		int iPos = FindSpan ( m_dStored, tItem.m_uUID );
		if ( iPos==-1 )
		{
			m_dStored.Add ( tItem );

		} else if ( m_dStored[iPos].m_uUID==tItem.m_uUID && !bReplace )
		{
			bAdded = false;
			sError.SetSprintf ( "duplicate id '" UINT64_FMT "'", tItem.m_uUID );
			SafeDelete ( pStored );
		} else if ( m_dStored[iPos].m_uUID==tItem.m_uUID && bReplace )
		{
			SafeDelete ( m_dStored[iPos].m_pQuery );
			m_dStored[iPos].m_pQuery = tItem.m_pQuery;

		} else
		{
			m_dStored.Insert ( iPos+1, tItem );
		}
	}
	if ( bAdded )
		m_iTID++;

	return bAdded;
}

int PercolateIndex_c::DeleteQueries ( const uint64_t * pQueries, int iCount )
{
	assert ( !iCount || pQueries!=NULL );

	int iDeleted = 0;
	ScWL_t wLock (m_tLock);

	for ( int i=0; i<iCount; i++ )
	{
		const StoredQueryKey_t * ppElem = m_dStored.BinarySearch ( bind ( &StoredQueryKey_t::m_uUID ), pQueries[i] );
		if ( ppElem )
		{
			int iElem = ppElem - m_dStored.Begin();
			SafeDelete ( m_dStored[iElem].m_pQuery );
			m_dStored.Remove ( iElem );
			iDeleted++;
		}
	}
	if ( iDeleted )
		m_iTID++;

	return iDeleted;
}

int PercolateIndex_c::DeleteQueries ( const char * sTags )
{
	CSphVector<uint64_t> dTags;
	PercolateTags ( sTags, dTags );

	if ( !dTags.GetLength() )
		return 0;

	int iDeleted = 0;
	ScWL_t wLock ( m_tLock );

	ARRAY_FOREACH ( i, m_dStored )
	{
		const StoredQuery_t * pQuery = m_dStored[i].m_pQuery;
		if ( !pQuery->m_dTags.GetLength() )
			continue;

		if ( TagsMatched ( dTags, pQuery->m_dTags ) )
		{
			SafeDelete ( m_dStored[i].m_pQuery );
			m_dStored.Remove ( i );
			i--;
			iDeleted++;
		}
	}
	if ( iDeleted )
		++m_iTID;

	return iDeleted;
}

void PercolateIndex_c::Commit ( int * pDeleted, ISphRtAccum * pAccExt )
{
	if ( pDeleted )
		*pDeleted = m_iDeleted;
	m_iDeleted = 0;
	RollBack ( pAccExt );
}

bool PercolateIndex_c::DeleteDocument ( const SphDocID_t * pUIDS, int iCount, CSphString &, ISphRtAccum * pAccExt )
{
	assert ( !iCount || pUIDS!=NULL );

	int iDeleted = 0;
	ScWL_t wLock ( m_tLock );

	for ( int i = 0; i<iCount; ++i )
	{
		const StoredQueryKey_t * ppElem = m_dStored.BinarySearch ( bind ( &StoredQueryKey_t::m_uUID ), pUIDS[i] );
		if ( ppElem )
		{
			int iElem = ppElem - m_dStored.Begin ();
			SafeDelete ( m_dStored[iElem].m_pQuery );
			m_dStored.Remove ( iElem );
			++iDeleted;
		}
	}
	if ( iDeleted )
	{
		++m_iTID;
		m_iDeleted = iDeleted;
	}


//	return iDeleted;
//	RollBack ( pAccExt );
	return true;
}

bool PercolateIndex_c::MultiScan ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters,
	ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs &tArgs ) const
{
	assert ( tArgs.m_iTag>=0 );

	// we count documents only (before filters)
	if ( pQuery->m_iMaxPredictedMsec )
		pResult->m_bHasPrediction = true;

	if ( tArgs.m_uPackedFactorFlags & SPH_FACTOR_ENABLE )
		pResult->m_sWarning.SetSprintf ( "packedfactors() will not work with a fullscan; you need to specify a query" );

	// start counting
	int64_t tmQueryStart = sphMicroTimer ();
	int64_t tmMaxTimer = 0;
	if ( pQuery->m_uMaxQueryMsec>0 )
		tmMaxTimer = sphMicroTimer () + pQuery->m_uMaxQueryMsec * 1000; // max_query_time

	// select the sorter with max schema
	// uses GetAttrsCount to get working facets (was GetRowSize)
	int iMaxSchemaSize = -1;
	int iMaxSchemaIndex = -1;
	for ( int i = 0; i<iSorters; ++i )
		if ( ppSorters[i]->GetSchema ()->GetAttrsCount ()>iMaxSchemaSize )
		{
			iMaxSchemaSize = ppSorters[i]->GetSchema ()->GetAttrsCount ();
			iMaxSchemaIndex = i;
		}

	const ISphSchema &tMaxSorterSchema = *( ppSorters[iMaxSchemaIndex]->GetSchema () );

	CSphVector<const ISphSchema *> dSorterSchemas;
	SorterSchemas ( ppSorters, iSorters, iMaxSchemaIndex, dSorterSchemas );

	// setup calculations and result schema
	CSphQueryContext tCtx ( *pQuery );
	if ( !tCtx.SetupCalc ( pResult, tMaxSorterSchema, m_tMatchSchema, nullptr, false, dSorterSchemas ) )
		return false;

	// setup filters
	CreateFilterContext_t tFlx;
	tFlx.m_pFilters = &pQuery->m_dFilters;
	tFlx.m_pFilterTree = &pQuery->m_dFilterTree;
	tFlx.m_pSchema = &tMaxSorterSchema;
	tFlx.m_eCollation = pQuery->m_eCollation;
	tFlx.m_bScan = true;

	if ( !tCtx.CreateFilters ( tFlx, pResult->m_sError, pResult->m_sWarning ) )
		return false;

	// setup lookup
	tCtx.m_bLookupFilter = false;
	tCtx.m_bLookupSort = true;


	// setup overrides
	if ( !tCtx.SetupOverrides ( pQuery, pResult, m_tMatchSchema, tMaxSorterSchema ) )
		return false;

	// get all locators
	const CSphColumnInfo & dUID = m_tMatchSchema.GetAttr ( 0 );
	const CSphColumnInfo & dColQuery = m_tMatchSchema.GetAttr ( 1 );
	const CSphColumnInfo & dColTags = m_tMatchSchema.GetAttr ( 2 );
	const CSphColumnInfo & dColFilters = m_tMatchSchema.GetAttr ( 3 );
	StringBuilder_c sFilters;

	// prepare to work them rows
	bool bRandomize = ppSorters[0]->m_bRandomize;

	CSphMatch tMatch;
	tMatch.Reset ( tMaxSorterSchema.GetDynamicSize () );
	tMatch.m_iWeight = tArgs.m_iIndexWeight;
	// fixme! tag also used over bitmask | 0x80000000,
	// which marks that match comes from remote.
	// using -1 might be also interpreted as 0xFFFFFFFF in such context!
	// Does it intended?
	tMatch.m_iTag = tCtx.m_dCalcFinal.GetLength () ? -1 : tArgs.m_iTag;


	if ( pResult->m_pProfile )
		pResult->m_pProfile->Switch ( SPH_QSTATE_FULLSCAN );

	int iCutoff = ( pQuery->m_iCutoff<=0 ) ? -1 : pQuery->m_iCutoff;
	BYTE * pData = nullptr;

	CSphVector<PercolateQueryDesc> dQueries;

	for ( const StoredQueryKey_t &dQuery : m_dStored )
	{
		auto * pQuery = dQuery.m_pQuery;

		tMatch.m_uDocID = dQuery.m_uUID;
		tMatch.SetAttr ( dUID.m_tLocator, dQuery.m_uUID );

		int iLen = pQuery->m_sQuery.Length ();
		tMatch.SetAttr ( dColQuery.m_tLocator, (SphAttr_t) sphPackPtrAttr ( iLen, pData ) );
		memcpy ( pData, pQuery->m_sQuery.cstr (), iLen );

		if ( pQuery->m_sTags.IsEmpty () )
			tMatch.SetAttr ( dColTags.m_tLocator, ( SphAttr_t ) 0 );
		else {
			iLen = pQuery->m_sTags.Length();
			tMatch.SetAttr ( dColTags.m_tLocator, ( SphAttr_t ) sphPackPtrAttr ( iLen, pData ) );
			memcpy ( pData, pQuery->m_sTags.cstr (), iLen );
		}

		sFilters.Clear ();
		if ( pQuery->m_dFilters.GetLength () )
			FormatFiltersQL ( pQuery->m_dFilters, pQuery->m_dFilterTree, sFilters );
		iLen = sFilters.GetLength ();
		tMatch.SetAttr ( dColFilters.m_tLocator, ( SphAttr_t ) sphPackPtrAttr ( iLen, pData ) );
		memcpy ( pData, sFilters.cstr (), iLen );


		++pResult->m_tStats.m_iFetchedDocs;

		tCtx.CalcFilter ( tMatch );
		if ( tCtx.m_pFilter && !tCtx.m_pFilter->Eval ( tMatch ) )
		{
			tCtx.FreeDataFilter ( tMatch );
			continue;
		}

		if ( bRandomize )
			tMatch.m_iWeight = ( sphRand () & 0xffff ) * tArgs.m_iIndexWeight;

		// submit match to sorters
		tCtx.CalcSort ( tMatch );

		bool bNewMatch = false;
		for ( int iSorter = 0; iSorter<iSorters; ++iSorter )
			bNewMatch |= ppSorters[iSorter]->Push ( tMatch );

		// stringptr expressions should be duplicated (or taken over) at this point
		tCtx.FreeDataFilter ( tMatch );
		tCtx.FreeDataSort ( tMatch );

		// handle cutoff
		if ( bNewMatch && --iCutoff==0 )
			break;

		// handle timer
		if ( tmMaxTimer && sphMicroTimer ()>=tmMaxTimer )
		{
			pResult->m_sWarning = "query time exceeded max_query_time";
			break;
		}
	}


	if ( pResult->m_pProfile )
		pResult->m_pProfile->Switch ( SPH_QSTATE_FINALIZE );

	pResult->m_iQueryTime += ( int ) ( ( sphMicroTimer () - tmQueryStart ) / 1000 );
	pResult->m_iBadRows += tCtx.m_iBadRows;

	return true; // fixme! */
}

bool PercolateIndex_c::MultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters,
									ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs &tArgs ) const
{
	assert ( pQuery );
	CSphQueryProfile * pProfile = pResult->m_pProfile;

	MEMORY ( MEM_DISK_QUERY );

	// to avoid the checking of a ppSorters's element for NULL on every next step, just filter out all nulls right here
	CSphVector<ISphMatchSorter *> dSorters;
	dSorters.Reserve ( iSorters );
	for ( int i = 0; i<iSorters; ++i )
		if ( ppSorters[i] )
			dSorters.Add ( ppSorters[i] );

	iSorters = dSorters.GetLength ();

	// if we have anything to work with
	if ( iSorters==0 )
		return false;

	// non-random at the start, random at the end
	dSorters.Sort ( CmpPSortersByRandom_fn () );

	const QueryParser_i * pQueryParser = pQuery->m_pQueryParser;
	assert ( pQueryParser );

	// fast path for scans
	if ( pQueryParser->IsFullscan ( *pQuery ) )
		return MultiScan ( pQuery, pResult, iSorters, &dSorters[0], tArgs );

	return false;
}

bool PercolateIndex_c::MultiQueryEx ( int iQueries, const CSphQuery * ppQueries, CSphQueryResult ** ppResults,
										ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs &tArgs) const
{
	bool bResult = false;
	for ( int i = 0; i<iQueries; ++i )
		if ( MultiQuery ( &ppQueries[i], ppResults[i], 1, &ppSorters[i], tArgs ) )
			bResult = true;
		else
			ppResults[i]->m_iMultiplier = -1;

	return bResult;
}

void PercolateIndex_c::PostSetup()
{
	PercolateIndex_i::PostSetup();
	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();

	// bigram filter
	if ( m_tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE && m_tSettings.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		m_pTokenizer->SetBuffer ( (BYTE*)m_tSettings.m_sBigramWords.cstr(), m_tSettings.m_sBigramWords.Length() );

		BYTE * pTok = NULL;
		while ( ( pTok = m_pTokenizer->GetToken() )!=NULL )
			m_tSettings.m_dBigramWords.Add() = (const char*)pTok;

		m_tSettings.m_dBigramWords.Sort();
	}

	// FIXME!!! handle error
	m_pTokenizerIndexing = m_pTokenizer->Clone ( SPH_CLONE_INDEX );
	ISphTokenizerRefPtr_c pIndexing { ISphTokenizer::CreateBigramFilter ( m_pTokenizerIndexing, m_tSettings.m_eBigramIndex, m_tSettings.m_sBigramWords, m_sLastError ) };
	if ( pIndexing )
		m_pTokenizerIndexing = pIndexing;

	// create queries
	ISphTokenizerRefPtr_c pTokenizer { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
	sphSetupQueryTokenizer ( pTokenizer, IsStarDict(), m_tSettings.m_bIndexExactWords, false );

	ISphTokenizerRefPtr_c pTokenizerJson { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
	sphSetupQueryTokenizer ( pTokenizerJson, IsStarDict(), m_tSettings.m_bIndexExactWords, true );

	CSphDictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( IsStarDict () )
		SetupStarDict ( pDict, pTokenizer );

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pTokenizer );

	CSphString sError;
	ARRAY_FOREACH ( i, m_dLoadedQueries )
	{
		const StoredQuery_t & tQuery = m_dLoadedQueries[i];
		const ISphTokenizer * pTok = tQuery.m_bQL ? pTokenizer : pTokenizerJson;
		uint64_t uUID = tQuery.m_uUID;
		bool bLoaded = AddQuery ( tQuery.m_sQuery.cstr(), tQuery.m_sTags.cstr(), &tQuery.m_dFilters, &tQuery.m_dFilterTree, false, tQuery.m_bQL, uUID, pTok, pDict, sError );
		if ( !bLoaded )
			sphWarning ( "index '%s': %d (id=" UINT64_FMT ") query failed to load, ignoring", m_sIndexName.cstr(), i, tQuery.m_uUID );
	}

	m_dLoadedQueries.Reset ( 0 );
	m_tmSaved = sphMicroTimer();
	m_iSavedTID = m_iTID;
}

bool PercolateIndex_c::Prealloc ( bool bStripPath )
{
	CSphString sLock;
	sLock.SetSprintf ( "%s.lock", m_sFilename.cstr() );
	m_iLockFD = ::open ( sLock.cstr(), SPH_O_NEW, 0644 );
	if ( m_iLockFD < 0 )
	{
		m_sLastError.SetSprintf ( "failed to open %s: %s", sLock.cstr(), strerrorm( errno ) );
		return false;
	}
	if ( !sphLockEx ( m_iLockFD, false ) )
	{
		m_sLastError.SetSprintf ( "failed to lock %s: %s", sLock.cstr(), strerrorm( errno ) );
		::close ( m_iLockFD );
		return false;
	}

	/////////////
	// load meta
	/////////////

	CSphString sMeta;
	sMeta.SetSprintf ( "%s.meta", m_sFilename.cstr() );

	// no readable meta? no disk part yet
	if ( !sphIsReadable ( sMeta.cstr() ) )
		return true;

	// opened and locked, lets read
	CSphAutoreader rdMeta;
	if ( !rdMeta.Open ( sMeta, m_sLastError ) )
		return false;

	if ( rdMeta.GetDword()!=META_HEADER_MAGIC )
	{
		m_sLastError.SetSprintf ( "invalid meta file %s", sMeta.cstr() );
		return false;
	}
	DWORD uVersion = rdMeta.GetDword();
	if ( uVersion==0 || uVersion>META_VERSION )
	{
		m_sLastError.SetSprintf ( "%s is v.%d, binary is v.%d", sMeta.cstr(), uVersion, META_VERSION );
		return false;
	}

	DWORD uIndexVersion = rdMeta.GetDword();

	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;

	// load settings
	ReadSchema ( rdMeta, m_tSchema, uIndexVersion, false );
	LoadIndexSettings ( m_tSettings, rdMeta, uIndexVersion );
	if ( !LoadTokenizerSettings ( rdMeta, tTokenizerSettings, tEmbeddedFiles, uIndexVersion, m_sLastError ) )
		return false;
	LoadDictionarySettings ( rdMeta, tDictSettings, tEmbeddedFiles, uIndexVersion, m_sLastWarning );

	// initialize AOT if needed
	DWORD uPrevAot = m_tSettings.m_uAotFilterMask;
	m_tSettings.m_uAotFilterMask = sphParseMorphAot ( tDictSettings.m_sMorphology.cstr() );
	if ( m_tSettings.m_uAotFilterMask!=uPrevAot )
		sphWarning ( "index '%s': morphology option changed from config has no effect, ignoring", m_sIndexName.cstr() );

	if ( bStripPath )
	{
		StripPath ( tTokenizerSettings.m_sSynonymsFile );
		StripPath ( tDictSettings.m_sStopwords );
		ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
			StripPath ( tDictSettings.m_dWordforms[i] );
	}

	// recreate tokenizer
	m_pTokenizer = ISphTokenizer::Create ( tTokenizerSettings, &tEmbeddedFiles, m_sLastError );
	if ( !m_pTokenizer )
		return false;

	// recreate dictionary
	m_pDict = sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, m_pTokenizer, m_sIndexName.cstr(), m_sLastError );
	if ( !m_pDict )
	{
		m_sLastError.SetSprintf ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );
		return false;
	}

	m_pTokenizer = ISphTokenizer::CreateMultiformFilter ( m_pTokenizer, m_pDict->GetMultiWordforms () );

	// regexp and RLP
	if ( uVersion>=6 )
	{
		ISphFieldFilterRefPtr_c pFieldFilter;
		CSphFieldFilterSettings tFieldFilterSettings;
		LoadFieldFilterSettings ( rdMeta, tFieldFilterSettings );
		if ( tFieldFilterSettings.m_dRegexps.GetLength() )
			pFieldFilter = sphCreateRegexpFilter ( tFieldFilterSettings, m_sLastError );

		if ( !sphSpawnRLPFilter ( pFieldFilter, m_tSettings, tTokenizerSettings, sMeta.cstr(), m_sLastError ) )
			return false;

		SetFieldFilter ( pFieldFilter );
	}

	// queries
	DWORD uQueries = rdMeta.GetDword();
	m_dLoadedQueries.Reset ( uQueries );
	ARRAY_FOREACH ( i, m_dLoadedQueries )
	{
		StoredQuery_t & tQuery = m_dLoadedQueries[i];

		if ( uVersion>=3 )
			tQuery.m_uUID = rdMeta.GetOffset();
		if ( uVersion>=4 )
			tQuery.m_bQL = ( rdMeta.GetDword()!=0 );

		tQuery.m_sQuery = rdMeta.GetString();
		if ( uVersion==1 )
			continue;

		tQuery.m_sTags = rdMeta.GetString();

		tQuery.m_dFilters.Resize ( rdMeta.GetDword() );
		tQuery.m_dFilterTree.Resize ( rdMeta.GetDword() );
		ARRAY_FOREACH ( iFilter, tQuery.m_dFilters )
		{
			CSphFilterSettings & tFilter = tQuery.m_dFilters[iFilter];
			tFilter.m_sAttrName = rdMeta.GetString();
			tFilter.m_bExclude = ( rdMeta.GetDword()!=0 );
			tFilter.m_bHasEqualMin = ( rdMeta.GetDword()!=0 );
			tFilter.m_bHasEqualMax = ( rdMeta.GetDword()!=0 );
			tFilter.m_eType = (ESphFilter)rdMeta.GetDword();
			tFilter.m_eMvaFunc = (ESphMvaFunc)rdMeta.GetDword ();
			rdMeta.GetBytes ( &tFilter.m_iMinValue, sizeof(tFilter.m_iMinValue) );
			rdMeta.GetBytes ( &tFilter.m_iMaxValue, sizeof(tFilter.m_iMaxValue) );
			tFilter.m_dValues.Resize ( rdMeta.GetDword() );
			tFilter.m_dStrings.Resize ( rdMeta.GetDword() );
			ARRAY_FOREACH ( j, tFilter.m_dValues )
				rdMeta.GetBytes ( tFilter.m_dValues.Begin() + j, sizeof ( tFilter.m_dValues[j] ) );
			ARRAY_FOREACH ( j, tFilter.m_dStrings )
				tFilter.m_dStrings[j] = rdMeta.GetString();
		}
		ARRAY_FOREACH ( iTree, tQuery.m_dFilterTree )
		{
			FilterTreeItem_t & tItem = tQuery.m_dFilterTree[iTree];
			tItem.m_iLeft = rdMeta.GetDword();
			tItem.m_iRight = rdMeta.GetDword();
			tItem.m_iFilterItem = rdMeta.GetDword();
			tItem.m_bOr = ( rdMeta.GetDword()!=0 );
		}
	}
	m_tmSaved = sphMicroTimer();
	m_iTID = m_iSavedTID = 1;

	return true;
}

void PercolateIndex_c::SaveMeta()
{
	// sanity check
	if ( m_iLockFD < 0 )
		return;

	// write new meta
	CSphString sMeta, sMetaNew;
	sMeta.SetSprintf ( "%s.meta", m_sFilename.cstr() );
	sMetaNew.SetSprintf ( "%s.meta.new", m_sFilename.cstr() );

	CSphString sError;
	CSphWriter wrMeta;
	if ( !wrMeta.OpenFile ( sMetaNew, sError ) )
	{
		sphWarning ( "failed to serialize meta: %s", sError.cstr() );
		return;
	}

	wrMeta.PutDword ( META_HEADER_MAGIC );
	wrMeta.PutDword ( META_VERSION );
	wrMeta.PutDword ( INDEX_FORMAT_VERSION );

	WriteSchema ( wrMeta, m_tSchema );
	SaveIndexSettings ( wrMeta, m_tSettings );
	SaveTokenizerSettings ( wrMeta, m_pTokenizer, m_tSettings.m_iEmbeddedLimit );
	SaveDictionarySettings ( wrMeta, m_pDict, false, m_tSettings.m_iEmbeddedLimit );

	// meta v.6
	SaveFieldFilterSettings ( wrMeta, m_pFieldFilter );

	Verify ( m_tLock.ReadLock() );
	wrMeta.PutDword ( m_dStored.GetLength() );

	for ( const auto & dStored : m_dStored )
	{
		const StoredQuery_t * pQuery = dStored.m_pQuery;
		wrMeta.PutOffset ( pQuery->m_uUID );
		wrMeta.PutDword ( !!pQuery->m_bQL );
		wrMeta.PutString ( pQuery->m_sQuery );
		wrMeta.PutString ( pQuery->m_sTags );
		wrMeta.PutDword ( pQuery->m_dFilters.GetLength() );
		wrMeta.PutDword ( pQuery->m_dFilterTree.GetLength() );
		for ( const CSphFilterSettings &tFilter : pQuery->m_dFilters )
		{
			wrMeta.PutString ( tFilter.m_sAttrName );
			wrMeta.PutDword ( !!tFilter.m_bExclude );
			wrMeta.PutDword ( !!tFilter.m_bHasEqualMin );
			wrMeta.PutDword ( !!tFilter.m_bHasEqualMax );
			wrMeta.PutDword ( tFilter.m_eType );
			wrMeta.PutDword ( tFilter.m_eMvaFunc );
			wrMeta.PutBytes ( &tFilter.m_iMinValue, sizeof(tFilter.m_iMinValue) );
			wrMeta.PutBytes ( &tFilter.m_iMaxValue, sizeof(tFilter.m_iMaxValue) );
			wrMeta.PutDword ( tFilter.m_dValues.GetLength() );
			wrMeta.PutDword ( tFilter.m_dStrings.GetLength() );
			wrMeta.PutBytes ( tFilter.m_dValues.begin(), tFilter.m_dValues.GetLengthBytes());
			for ( const CSphString& sString : tFilter.m_dStrings )
				wrMeta.PutString ( sString );
		}
		for ( const FilterTreeItem_t &tItem : pQuery->m_dFilterTree )
		{
			wrMeta.PutDword ( tItem.m_iLeft );
			wrMeta.PutDword ( tItem.m_iRight );
			wrMeta.PutDword ( tItem.m_iFilterItem );
			wrMeta.PutDword ( tItem.m_bOr );
		}
	}

	m_iSavedTID = m_iTID;
	m_tmSaved = sphMicroTimer();

	m_tLock.Unlock();

	wrMeta.CloseFile();

	// rename
	if ( sph::rename ( sMetaNew.cstr(), sMeta.cstr() ) )
		sphWarning ( "failed to rename meta (src=%s, dst=%s, errno=%d, error=%s)", sMetaNew.cstr(), sMeta.cstr(), errno, strerrorm( errno ) );
}

void PercolateIndex_c::GetQueries ( const char * sFilterTags, bool bTagsEq, const CSphFilterSettings * pUID, int iOffset, int iLimit, CSphVector<PercolateQueryDesc> & dQueries )
{
	// FIXME!!! move to filter, add them via join
	CSphVector<uint64_t> dTags;
	PercolateTags ( sFilterTags, dTags );

	// FIXME!!! add UID scan for UID IN (value list) queries
	CSphScopedPtr<PercolateFilter_i> tFilter ( CreatePercolateFilter ( pUID ) );

	// reserve size to store all queries
	if ( !dTags.GetLength() && !tFilter.Ptr() )
		dQueries.Reserve ( m_dStored.GetLength() );

	StringBuilder_c tBuf;
	ScRL_t rLock ( m_tLock );

	int iFrom = 0;
	if ( iLimit>0 && iOffset>0 )
		iFrom = Min ( iOffset, m_dStored.GetLength() );

	for ( int i=iFrom; i<m_dStored.GetLength(); ++i )
	{
		const StoredQuery_t * pQuery = m_dStored[i].m_pQuery;
		if ( dTags.GetLength() )
		{
			if ( !pQuery->m_dTags.GetLength() )
				continue;

			if ( !TagsMatched ( dTags, pQuery->m_dTags, bTagsEq ) )
				continue;
		}

		if ( tFilter.Ptr() && !tFilter->Eval ( pQuery->m_uUID ) )
			continue;

		PercolateQueryDesc & tItem = dQueries.Add();
		tItem.m_uID = pQuery->m_uUID;
		tItem.m_sQuery = pQuery->m_sQuery;
		tItem.m_sTags = pQuery->m_sTags;
		tItem.m_bQL = pQuery->m_bQL;

		if ( pQuery->m_dFilters.GetLength() )
		{
			tBuf.Clear();
			FormatFiltersQL ( pQuery->m_dFilters, pQuery->m_dFilterTree, tBuf );
			tItem.m_sFilters = tBuf.cstr();
		}

		if ( iLimit>0 && dQueries.GetLength()==iLimit )
			break;
	}
}

bool PercolateIndex_c::Truncate ( CSphString & )
{
	{
		ScWL_t wLock ( m_tLock );
		for ( auto& dStored : m_dStored )
			SafeDelete ( dStored.m_pQuery );
		m_dStored.Reset();
	}
	++m_iTID;

	SaveMeta();
	return true;
}

void PercolateMatchResult_t::Swap ( PercolateMatchResult_t & tOther )
{
	m_bGetDocs = tOther.m_bGetDocs;
	m_dDocs.SwapData ( tOther.m_dDocs );
	m_iQueriesMatched = tOther.m_iQueriesMatched;
	m_iDocsMatched = tOther.m_iDocsMatched;
	m_tmTotal = tOther.m_tmTotal;
	m_iQueriesFailed = tOther.m_iQueriesFailed;

	m_bVerbose = tOther.m_bVerbose;
	m_dQueryDT.SwapData ( tOther.m_dQueryDT );
	m_iEarlyOutQueries = tOther.m_iEarlyOutQueries;
	m_iTotalQueries = tOther.m_iTotalQueries;
	m_iOnlyTerms = tOther.m_iOnlyTerms;
	m_tmSetup = tOther.m_tmSetup;
}

void FixPercolateSchema ( CSphSchema & tSchema )
{
	if ( !tSchema.GetFieldsCount() )
		tSchema.AddField ( CSphColumnInfo ( "text" ) );

	if ( !tSchema.GetAttrsCount() )
	{
		CSphColumnInfo tCol ( "gid", SPH_ATTR_INTEGER );
		tCol.m_tLocator = CSphAttrLocator();
		tSchema.AddAttr ( tCol, false );
	}
}

bool PercolateIndex_c::IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError ) const
{
	tSetup.m_tSchema = tSettings.m_tSchema;
	FixPercolateSchema ( tSetup.m_tSchema );

	CSphString sTmp;
	bool bSameSchema = m_tSchema.CompareTo ( tSettings.m_tSchema, sTmp, false );

	return CreateReconfigure ( m_sIndexName, IsStarDict(), m_pFieldFilter, m_tSettings,
		m_pTokenizer->GetSettingsFNV(), m_pDict->GetSettingsFNV(), m_pTokenizer->GetMaxCodepointLength(), bSameSchema, tSettings, tSetup, sError );
}

void PercolateIndex_c::Reconfigure ( CSphReconfigureSetup & tSetup )
{
	m_tSchema = tSetup.m_tSchema;

	Setup ( tSetup.m_tIndex );
	SetTokenizer ( tSetup.m_pTokenizer );
	SetDictionary ( tSetup.m_pDict );
	SetFieldFilter ( tSetup.m_pFieldFilter );

	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();
	SetupQueryTokenizer();

	m_dLoadedQueries.Reset ( m_dStored.GetLength() );
	ARRAY_FOREACH ( i, m_dLoadedQueries )
	{
		StoredQuery_t & tQuery = m_dLoadedQueries[i];
		const StoredQuery_t * pStored = m_dStored[i].m_pQuery;

		tQuery.m_uUID = pStored->m_uUID;
		tQuery.m_sQuery = pStored->m_sQuery;
		tQuery.m_sTags = pStored->m_sTags;
		tQuery.m_dFilters = pStored->m_dFilters;
		tQuery.m_dFilterTree = pStored->m_dFilterTree;

		SafeDelete ( pStored );
	}
	m_dStored.Resize ( 0 );
	m_iTID++;

	PostSetup();
}

void SetPercolateThreads ( int iThreads )
{
	g_iPercolateThreads = Max ( 1, iThreads );
}

void PercolateIndex_c::ForceRamFlush ( bool bPeriodic )
{
	if ( m_iTID<=m_iSavedTID )
		return;

	int64_t tmStart = sphMicroTimer();
	int64_t iWasTID = m_iSavedTID;
	int64_t tmWas = m_tmSaved;
	SaveMeta ();

	int64_t tmNow = sphMicroTimer();
	int64_t tmAge = tmNow - tmWas;
	int64_t tmSave = tmNow - tmStart;

	sphInfo ( "percolate: index %s: saved ok (mode=%s, last TID=" INT64_FMT ", current TID=" INT64_FMT ", "
		"time delta=%d sec, took=%d.%03d sec)"
		, m_sIndexName.cstr(), bPeriodic ? "periodic" : "forced"
		, iWasTID, m_iTID
		, (int) (tmAge/1000000), (int)(tmSave/1000000), (int)((tmSave/1000)%1000) );
}

void PercolateIndex_c::ForceDiskChunk ()
{
	ForceRamFlush ( false );
}
