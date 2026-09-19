/* Omacall's build of pjproject: audio only, one account, one call at a time.
 *
 * Everything a ./configure flag can express lives in the PKGBUILD next to this
 * file. What is left here are the two compile-time choices those flags cannot
 * make.
 */

/* WebRTC's echo canceller ships inside pjproject's third_party tree and is
 * built unconditionally, but the header leaves it off. Omacall runs on laptop
 * speakers as often as on a headset, so turn it on. AEC3 stays off on purpose:
 * it is C++, and linking libstdc++ into a C daemon is a real cost for a gain we
 * have not measured. */
#define PJMEDIA_HAS_WEBRTC_AEC      1

/* No video in v1, not even as dead code. The configure flags already exclude
 * it; saying so here keeps the intent in the one file a packager reads. */
#define PJMEDIA_HAS_VIDEO           0
