/**
 * EGM96 geoid undulation calculator (browser port of the
 * `earthgravitymodel1996` npm package, rewritten to use fetch instead of fs).
 *
 * Loads the WW15MGH.DAC grid (2 MB, big-endian Int16, 1440×721) once and
 * bilinearly interpolates the geoid undulation N at a given lat/lon (degrees).
 *
 *   h_orthometric = h_ellipsoidal - N
 *
 * where N is the height of the geoid (mean sea level) above the WGS84
 * ellipsoid. N is negative where the geoid lies below the ellipsoid.
 */
(function () {
  "use strict";
  const DEG2RAD = Math.PI / 180.0;

  let dataPromise = null;

  function loadData() {
    if (dataPromise) return dataPromise;
    dataPromise = fetch("vendor/WW15MGH.DAC")
      .then((r) => {
        if (!r.ok) throw new Error("EGM96 grid HTTP " + r.status);
        return r.arrayBuffer();
      })
      .then((buf) => {
        // Grid is big-endian Int16; browsers are little-endian → swap bytes.
        const byteView = new Uint8Array(buf);
        for (let k = 0; k < byteView.length; k += 2) {
          const tmp = byteView[k];
          byteView[k] = byteView[k + 1];
          byteView[k + 1] = tmp;
        }
        return new Int16Array(buf, 0, buf.byteLength / 2);
      });
    return dataPromise;
  }

  // Grid value in centimeters. heightIndex wraps around (longitude is cyclic).
  function gridValue(data, recordIndex, heightIndex) {
    if (recordIndex < 0) recordIndex = 0;
    else if (recordIndex > 720) recordIndex = 720;
    if (heightIndex > 1439) heightIndex -= 1440;
    else if (heightIndex < 0) heightIndex += 1440;
    return data[recordIndex * 1440 + heightIndex];
  }

  /**
   * @param lon longitude in degrees
   * @param lat latitude in degrees
   * @returns {Promise<number>} geoid undulation N in meters (MSL above ellipsoid)
   */
  function getHeight(lon, lat) {
    return loadData().then((data) => {
      const latR = lat * DEG2RAD;
      const lonR = lon * DEG2RAD;

      const recordIndex = (720 * (Math.PI * 0.5 - latR)) / Math.PI;
      const ri = recordIndex < 0 ? 0 : recordIndex > 720 ? 720 : recordIndex;

      const twoPi = Math.PI * 2.0;
      const modTwoPi = ((lonR % twoPi) + twoPi) % twoPi;
      const lonNorm = Math.abs(modTwoPi) < 1e-14 && Math.abs(lonR) > 1e-14 ? twoPi : modTwoPi;

      const heightIndex = (1440 * lonNorm) / twoPi;
      const hi = heightIndex < 0 ? 0 : heightIndex > 1440 ? 1440 : heightIndex;

      const i = hi | 0;
      const j = ri | 0;
      const x = hi - i;
      const y = ri - j;
      const x2 = 1.0 - x;
      const y2 = 1.0 - y;

      const f11 = gridValue(data, j, i);
      const f21 = gridValue(data, j, i + 1);
      const f12 = gridValue(data, j + 1, i);
      const f22 = gridValue(data, j + 1, i + 1);

      return (f11 * x2 * y2 + f21 * x * y2 + f12 * x2 * y + f22 * x * y) / 100.0;
    });
  }

  window.EGM96 = { getHeight, loadData };
})();
