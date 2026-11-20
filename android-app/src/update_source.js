/**
 * The update source (the Hub's update_repo setting): where version checks and the icon
 * archive come from. Empty means the official OTA branch.
 */
export const DEFAULT_OTA_URL = 'https://raw.githubusercontent.com/iranl/harmony-ng/ota';

let current = '';

export function setOtaSource(url) {
  current = typeof url === 'string' ? url.trim() : '';
}

/** The source without a trailing slash or the old /bin/mips suffix (update.sh strips it too). */
export function otaSource() {
  return (current || DEFAULT_OTA_URL).replace(/\/+$/, '').replace(/\/bin\/mips$/, '').replace(/\/+$/, '');
}

/** GitHub releases API of a raw.githubusercontent.com source; '' for any other mirror. */
export function githubReleasesApi(src = otaSource()) {
  const m = /^https:\/\/raw\.githubusercontent\.com\/([^/]+)\/([^/]+)\//.exec(src + '/');
  return m ? `https://api.github.com/repos/${m[1]}/${m[2]}/releases/latest` : '';
}
