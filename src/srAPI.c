/*! \file srAPI.c
 *  \brief Implementation of the API
 *
 * \author  Rainer Gerhards <rgerhards@adiscon.com>
 * \date    2003-08-04
 *          Initial version (0.1) released.
 *
 * \date    2003-09-04
 *          begin major redesign so that multiple client profiles
 *          can be supported (the initial design just allowed one).
 *
 * Copyright 2002-2003 
 *     Rainer Gerhards and Adiscon GmbH. All Rights Reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 * 
 *     * Neither the name of Adiscon GmbH or Rainer Gerhards
 *       nor the names of its contributors may be used to
 *       endorse or promote products derived from this software without
 *       specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include "config.h"
#include "liblogging.h"
#include "beepsession.h"
#include "beepprofile.h"
#include "beeplisten.h"
#include "namevaluetree.h"
#include "beepprofile.h"
#include "clntprof-3195raw.h"
#include "clntprof-3195cooked.h"
#include "srAPI.h"

/* ################################################################# *
 * private members                                                   *
 * ################################################################# */

/**
 * This variable is a global library setting. It tells the library
 * if it should instruct the lib to call the OS socket initialiser.
 * Under Win32, for example, this is only allowed once per
 * process. So if the lib is integrated into a process that 
 * already uses sockets, the sockets initializer (WSAStartup()
 * in this case) should not be called.
 * This variable can be modified via a global setting.
 */
static int srAPI_bCallOSSocketInitializer = TRUE;

/**
 * Destructor for the API object.
 */
static void srAPIDestroy(srAPIObj* pThis)
{
	srAPICHECKVALIDOBJECT(pThis);

	if(pThis->pChan != NULL)
		sbSessCloseChan(pThis->pSess, pThis->pChan);

	if(pThis->pProfsSupported != NULL)
		sbNVTRDestroy(pThis->pProfsSupported);

	if(pThis->pSess != NULL)
		sbSessCloseSession(pThis->pSess);

	if(pThis->pLstn != NULL)
		sbLstnExit(pThis->pLstn);

	SRFREEOBJ(pThis);
}


/**
 * Add a profile object to the list of supported profiles.
 * \todo This method (and its helper FreeProf) is the same
 * as in the beeplisten object. We may want to merge these 
 * two into a single utility method...
 */
static srRetVal srAPIAddProfile(srAPIObj *pThis, sbProfObj *pProf)
{
	sbNVTEObj *pEntry;

	srAPICHECKVALIDOBJECT(pThis);
	sbProfCHECKVALIDOBJECT(pProf);
	sbNVTRCHECKVALIDOBJECT(pThis->pProfsSupported);

	if((pEntry = sbNVTAddEntry(pThis->pProfsSupported)) == NULL)
		return SR_RET_OUT_OF_MEMORY;
	sbNVTESetKeySZ(pEntry, pProf->pszProfileURI, TRUE);
	sbNVTESetUsrPtr(pEntry, pProf, sbProfDestroy);
	
	return SR_RET_OK;
}


/* ################################################################# *
 * public members                                                    *
 * ################################################################# */

srAPIObj* srAPIInitLib(void)
{
	srAPIObj *pThis;

	if((pThis = calloc(1, sizeof(srAPIObj))) == NULL)
		return NULL;

	pThis->OID = OIDsrAPI;
	pThis->pSess = NULL;
	pThis->pProfsSupported = NULL;
	pThis->pChan = NULL;
#	if FEATURE_LISTENER == 1
	pThis->OnSyslogMessageRcvd = NULL;
	pThis->pLstn = NULL;
#	endif
	sbSockLayerInit(srAPI_bCallOSSocketInitializer);

	return(pThis);
} 


srRetVal srAPISetOption(srAPIObj* pThis, SRoption iOpt, int iOptVal)
{
	switch(iOpt)
	{
	/* This must be done for all options, that require a pAPI
		if((pThis == NULL) || (pThis->OID != OIDsrAPI))
			return SR_RET_INVALID_HANDLE;
    */
	case srOPTION_CALL_OS_SOCKET_INITIALIZER:
		if(pThis != NULL)
			return SR_RET_INVALID_HANDLE;
		if(iOptVal != TRUE && iOptVal != FALSE)
			return SR_RET_INVALID_OPTVAL;
		srAPI_bCallOSSocketInitializer = iOptVal;
		break;
	default:
		return SR_RET_INVALID_LIB_OPTION;
	}

	return SR_RET_OK;
}


srRetVal srAPIExitLib(srAPIObj *pThis)
{
	if((pThis == NULL) || (pThis->OID != OIDsrAPI))
		return SR_RET_INVALID_HANDLE;

	srAPIDestroy(pThis);

	return SR_RET_OK;
}

/** 
 * Open a RFC 3195 / RAW log session.
 *
 * This API is effectively the initiator side of the RAW profile.
 *
 * This method looks like much code, but most of it is error 
 * handling ;) - so we keep it as a single long method.
 *
 * \param pszRemotePeer [in] The Peer to connect to.
 * \param iPort [in] The port the remote Peer is listening to.
 */
srRetVal srAPIOpenlog(srAPIObj *pThis, char* pszRemotePeer, int iPort)
{
	srRetVal iRet;
	sbProfObj *pProf;

	if((pThis == NULL) || (pThis->OID != OIDsrAPI))
		return SR_RET_INVALID_HANDLE;

	/* We need to create the list of supported profiles first.
	 */
	/** \todo later, make this depending on the lib option set by the user. */
	/* create profile list */
	if((pThis->pProfsSupported = sbNVTRConstruct()) == NULL)
		return SR_RET_OUT_OF_MEMORY;

	/* Now set up the profiles.
	 * IMPORTANT: the order of profile registration is also the
	 * profile priority. As such, preferred profiles should be 
	 * registered first. For example, we will register COOKED before
	 * RAW because we would like to select COOKED if both are available
	 * at the remote peer!
	 */
	/* set up the rfc 3195 COOKED profile */
	if((iRet = sbProfConstruct(&pProf, "http://xml.resource.org/profiles/syslog/COOKED")) != SR_RET_OK)
	{
		sbLstnDestroy(pThis->pLstn);
		return iRet;
	}

	if((iRet = sbProfSetAPIObj(pProf, pThis)) != SR_RET_OK)
	{
		srAPIDestroy(pThis);
		sbProfDestroy(pProf);
		return iRet;
	}

	if((iRet = sbProfSetClntEventHandlers(pProf, sbPSRCClntOpenLogChan, sbPSRCClntSendMsg, sbPSRCCOnClntCloseLogChan)) != SR_RET_OK)
	{
		sbProfDestroy(pProf);
		return iRet;
	}

	if((iRet = srAPIAddProfile(pThis, pProf)) != SR_RET_OK)
	{
		srAPIDestroy(pThis);
		sbProfDestroy(pProf);
		return iRet;
	}


	/* set up the rfc 3195/raw profile */
	if((iRet = sbProfConstruct(&pProf, "http://xml.resource.org/profiles/syslog/RAWx")) != SR_RET_OK)
	{
		sbLstnDestroy(pThis->pLstn);
		return iRet;
	}

	if((iRet = sbProfSetAPIObj(pProf, pThis)) != SR_RET_OK)
	{
		srAPIDestroy(pThis);
		sbProfDestroy(pProf);
		return iRet;
	}

	if((iRet = sbProfSetClntEventHandlers(pProf, sbPSSRClntOpenLogChan, sbPSSRClntSendMsg, sbPSSRCOnClntCloseLogChan)) != SR_RET_OK)
	{
		sbProfDestroy(pProf);
		return iRet;
	}

	if((iRet = srAPIAddProfile(pThis, pProf)) != SR_RET_OK)
	{
		srAPIDestroy(pThis);
		sbProfDestroy(pProf);
		return iRet;
	}

	/* OK, we got our housekeeping done, so let's talk to the peer )
	 */
	if((pThis->pSess = sbSessOpenSession(pszRemotePeer, iPort, pThis->pProfsSupported)) == NULL)
	{
		srAPIDestroy(pThis);
		return SR_RET_ERR;
	}

	if((pThis->pChan = sbSessOpenChan(pThis->pSess)) == NULL)
	{
		srAPIDestroy(pThis);
		return SR_RET_ERR;
	}

	/* Ok, we must now call the profile's new channel created handler */
	iRet = pThis->pChan->pProf->OnClntOpenLogChan(pThis->pChan, NULL);
	// destroy API object on failur!
	
//	sbMesgDestroy(pProfileGreeting);

	return iRet;
}


srRetVal srAPISendLogmsg(srAPIObj* pThis, char* szLogmsg)
{
	/* Attention: order of conditions is vitally important! */
	if((pThis == NULL) || (pThis->OID != OIDsrAPI))
		return SR_RET_INVALID_HANDLE;

	if(szLogmsg == NULL)
		return SR_RET_NULL_MSG_PROVIDED;

	assert(pThis->pChan->pProf->OnClntSendLogMsg != NULL);

	return pThis->pChan->pProf->OnClntSendLogMsg(pThis->pChan, szLogmsg);
}


srRetVal srAPICloseLog(srAPIObj *pThis)
{
	int iRet = SR_RET_OK;

	/* Attention: order of conditions is vitally important! */
	if((pThis == NULL) || (pThis->OID != OIDsrAPI))
		return SR_RET_INVALID_HANDLE;

	assert(pThis->pChan->pProf->OnClntCloseLogChan != NULL);
	iRet = pThis->pChan->pProf->OnClntCloseLogChan(pThis->pChan);

	/* now destroy all but the API object itself */
	if(pThis->pChan != NULL)
	{
		sbSessCloseChan(pThis->pSess, pThis->pChan);
		pThis->pChan = NULL;
	}

	if(pThis->pSess != NULL)
	{
		sbSessCloseSession(pThis->pSess);
		pThis->pSess = NULL;
	}

	return(iRet);
}


srRetVal srAPISetUsrPointer(srAPIObj *pAPI, void* pUsr)
{
	if((pAPI == NULL) || (pAPI->OID != OIDsrAPI))
		return SR_RET_INVALID_HANDLE;

	pAPI->pUsr = pUsr;

	return SR_RET_OK;
}


srRetVal srAPIGetUsrPointer(srAPIObj *pAPI, void **ppToStore)
{
	if((pAPI == NULL) || (pAPI->OID != OIDsrAPI))
		return SR_RET_INVALID_HANDLE;

	if(ppToStore == NULL)
		return SR_RET_INVALID_HANDLE;

	*ppToStore = pAPI->pUsr;

	return SR_RET_OK;
}