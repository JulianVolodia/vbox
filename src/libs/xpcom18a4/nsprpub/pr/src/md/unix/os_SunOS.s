/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* 
* The contents of this file are subject to the Mozilla Public
* License Version 1.1 (the "License"); you may not use this file
* except in compliance with the License. You may obtain a copy of
* the License at http://www.mozilla.org/MPL/
* 
* Software distributed under the License is distributed on an "AS
* IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
* implied. See the License for the specific language governing
* rights and limitations under the License.
* 
* The Original Code is the Netscape Portable Runtime (NSPR).
* 
* The Initial Developer of the Original Code is Netscape
* Communications Corporation.  Portions created by Netscape are 
* Copyright (C) 1998-2000 Netscape Communications Corporation.  All
* Rights Reserved.
* 
* Contributor(s):
* 
* Alternatively, the contents of this file may be used under the
* terms of the GNU General Public License Version 2 or later (the
* "GPL"), in which case the provisions of the GPL are applicable 
* instead of those above.  If you wish to allow use of your 
* version of this file only under the terms of the GPL and not to
* allow others to use your version of this file under the MPL,
* indicate your decision by deleting the provisions above and
* replace them with the notice and other provisions required by
* the GPL.  If you do not delete the provisions above, a recipient
* may use your version of this file under either the MPL or the
* GPL.
*/ 

	.text

/*
 * sol_getsp()
 *
 * Return the current sp (for debugging)
 */
	.global sol_getsp
sol_getsp:
	retl
   	mov     %sp, %o0


/*
 * sol_curthread()
 *
 * Return a unique identifier for the currently active thread.
 */
	.global sol_curthread
sol_curthread:
    retl
    mov %g7, %o0
                  

	.global __MD_FlushRegisterWindows
	.global _MD_FlushRegisterWindows

__MD_FlushRegisterWindows:
_MD_FlushRegisterWindows:

	ta	3
	ret
	restore

