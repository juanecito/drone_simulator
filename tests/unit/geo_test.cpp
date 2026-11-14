// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>

#include "ds/common/math.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/geo/ellipsoid.hpp"
#include "ds/geo/geodesic.hpp"
#include "ds/geo/utm.hpp"
#include "ds/geo/web_mercator.hpp"

#include "fixture_csv.hpp"

namespace {

using namespace ds::geo;
using ds::test::CsvTable;

constexpr double kMillimetre = 1e-3;
// 1 mm on the Earth's surface expressed in degrees of latitude.
constexpr double kMillimetreDeg = 1e-3 / 111000.0;

// --- Geodetic ⇄ ECEF (FR-GEO-01) --------------------------------------------------------------

TEST(GeoEcef, MatchesProjReference) {
    const CsvTable table("geo/ecef.csv");
    ASSERT_GT(table.size(), 100U);
    for (std::size_t i = 0; i < table.size(); ++i) {
        const Geodetic g{table.number(i, "lat_deg"), table.number(i, "lon_deg"),
                         table.number(i, "h_m")};
        const Ecef e = toEcef(g);
        EXPECT_NEAR(e.x, table.number(i, "x_m"), kMillimetre) << "row " << i;
        EXPECT_NEAR(e.y, table.number(i, "y_m"), kMillimetre) << "row " << i;
        EXPECT_NEAR(e.z, table.number(i, "z_m"), kMillimetre) << "row " << i;

        const Geodetic back = toGeodetic(e);
        EXPECT_NEAR(back.latitudeDeg, g.latitudeDeg, kMillimetreDeg) << "row " << i;
        if (std::abs(g.latitudeDeg) < 89.99) {  // Longitude is meaningless at the pole.
            EXPECT_NEAR(ds::common::wrapDegrees180(back.longitudeDeg - g.longitudeDeg), 0.0,
                        kMillimetreDeg / std::cos(ds::common::degToRad(g.latitudeDeg)))
                << "row " << i;
        }
        EXPECT_NEAR(back.heightM, g.heightM, kMillimetre) << "row " << i;
    }
}

TEST(GeoEcef, PolesAreHandled) {
    const Ecef northPole{0.0, 0.0, kWgs84.b() + 100.0};
    const Geodetic g = toGeodetic(northPole);
    EXPECT_DOUBLE_EQ(g.latitudeDeg, 90.0);
    EXPECT_NEAR(g.heightM, 100.0, 1e-6);
}

// --- Local ENU frame (FR-GEO-01, FR-GEO-02, NFR-REAL-06) ----------------------------------------

TEST(GeoLocalFrame, OriginMapsToZero) {
    const LocalFrame frame({40.4168, -3.7038, 667.0});
    const Enu enu = frame.toEnu(frame.origin());
    EXPECT_NEAR(enu.east, 0.0, 1e-9);
    EXPECT_NEAR(enu.north, 0.0, 1e-9);
    EXPECT_NEAR(enu.up, 0.0, 1e-9);
}

TEST(GeoLocalFrame, AxesPointEastNorthUp) {
    const LocalFrame frame({45.0, 7.0, 0.0});
    const Enu north = frame.toEnu(Geodetic{45.001, 7.0, 0.0});
    EXPECT_GT(north.north, 100.0);
    EXPECT_NEAR(north.east, 0.0, 1e-6);
    const Enu east = frame.toEnu(Geodetic{45.0, 7.001, 0.0});
    EXPECT_GT(east.east, 70.0);
    EXPECT_NEAR(east.north, 0.0, 0.01);  // Only the meridian convergence term.
    const Enu up = frame.toEnu(Geodetic{45.0, 7.0, 25.0});
    EXPECT_NEAR(up.up, 25.0, 1e-9);
    EXPECT_NEAR(up.east, 0.0, 1e-9);
}

TEST(GeoLocalFrame, RoundTripWithin50kmIsBelowOneMillimetre) {
    for (const Geodetic origin : {Geodetic{40.4168, -3.7038, 667.0}, Geodetic{-54.8, -68.3, 10.0},
                                  Geodetic{69.65, 18.96, 0.0}, Geodetic{0.0, 179.99, 0.0}}) {
        const LocalFrame frame(origin);
        for (double east = -50000.0; east <= 50000.0; east += 12500.0) {
            for (double north = -50000.0; north <= 50000.0; north += 12500.0) {
                for (const double up : {-400.0, 0.0, 3000.0}) {
                    const Enu enu{east, north, up};
                    const Enu back = frame.toEnu(frame.toGeodetic(enu));
                    EXPECT_NEAR(back.east, east, kMillimetre);
                    EXPECT_NEAR(back.north, north, kMillimetre);
                    EXPECT_NEAR(back.up, up, kMillimetre);
                }
            }
        }
    }
}

TEST(GeoWorldFrame, IsYUpWithZSouth) {
    const WorldPosition w = toWorld(Enu{1.0, 2.0, 3.0});
    EXPECT_DOUBLE_EQ(w.x, 1.0);
    EXPECT_DOUBLE_EQ(w.y, 3.0);
    EXPECT_DOUBLE_EQ(w.z, -2.0);
    const Enu back = toEnu(w);
    EXPECT_DOUBLE_EQ(back.east, 1.0);
    EXPECT_DOUBLE_EQ(back.north, 2.0);
    EXPECT_DOUBLE_EQ(back.up, 3.0);
}

// --- UTM (FR-GEO-02a, NFR-REAL-07) --------------------------------------------------------------

TEST(GeoUtm, MatchesProjReferenceWithinOneMillimetre) {
    const CsvTable table("geo/utm.csv");
    ASSERT_GT(table.size(), 400U);
    for (std::size_t i = 0; i < table.size(); ++i) {
        const double lat = table.number(i, "lat_deg");
        const double lon = table.number(i, "lon_deg");
        const auto utm = toUtm(lat, lon);
        ASSERT_TRUE(utm.has_value()) << utm.error().message;
        EXPECT_EQ(utm->zone, static_cast<int>(table.number(i, "zone"))) << lat << ", " << lon;
        EXPECT_EQ(utm->north, table.text(i, "hemisphere") == "N");
        EXPECT_NEAR(utm->easting, table.number(i, "easting_m"), kMillimetre) << lat << ", " << lon;
        EXPECT_NEAR(utm->northing, table.number(i, "northing_m"), kMillimetre)
            << lat << ", " << lon;

        const auto back = fromUtm(*utm);
        ASSERT_TRUE(back.has_value());
        EXPECT_NEAR(back->latitudeDeg, lat, kMillimetreDeg) << lat << ", " << lon;
        EXPECT_NEAR(ds::common::wrapDegrees180(back->longitudeDeg - lon), 0.0,
                    kMillimetreDeg / std::cos(ds::common::degToRad(lat)))
            << lat << ", " << lon;
    }
}

TEST(GeoUtm, ZoneExceptions) {
    EXPECT_EQ(utmZone(60.0, 2.99), 31);
    EXPECT_EQ(utmZone(60.0, 3.0), 32);  // Norway: 32V widened west.
    EXPECT_EQ(utmZone(60.0, 11.99), 32);
    EXPECT_EQ(utmZone(55.99, 4.0), 31);
    EXPECT_EQ(utmZone(64.0, 4.0), 31);
    EXPECT_EQ(utmZone(78.0, 8.99), 31);  // Svalbard.
    EXPECT_EQ(utmZone(78.0, 9.0), 33);
    EXPECT_EQ(utmZone(78.0, 21.0), 35);
    EXPECT_EQ(utmZone(78.0, 33.0), 37);
    EXPECT_EQ(utmZone(78.0, 42.0), 38);
    EXPECT_EQ(utmZone(0.0, -180.0), 1);
    EXPECT_EQ(utmZone(0.0, 180.0), 1);  // 180° E is the same meridian as 180° W.
    EXPECT_EQ(utmZone(0.0, 179.99), 60);
}

TEST(GeoUtm, BandLetters) {
    EXPECT_EQ(utmBand(-80.0), 'C');
    EXPECT_EQ(utmBand(-0.0001), 'M');
    EXPECT_EQ(utmBand(0.0), 'N');
    EXPECT_EQ(utmBand(40.4), 'T');
    EXPECT_EQ(utmBand(71.99), 'W');
    EXPECT_EQ(utmBand(72.0), 'X');
    EXPECT_EQ(utmBand(84.0), 'X');
}

TEST(GeoUtm, RejectsPolarRegions) {
    EXPECT_FALSE(toUtm(84.01, 0.0).has_value());
    EXPECT_FALSE(toUtm(-80.01, 0.0).has_value());
    EXPECT_FALSE(toUtm(std::nan(""), 0.0).has_value());
    EXPECT_FALSE(toUtmInZone(10.0, 10.0, 61).has_value());
}

TEST(GeoUtm, NeighbouringZoneIsSupported) {
    // A drone just east of a zone border can still be expressed in the western zone.
    const auto own = toUtm(40.0, 6.01);
    const auto west = toUtmInZone(40.0, 6.01, 31);
    ASSERT_TRUE(own && west);
    EXPECT_EQ(own->zone, 32);
    EXPECT_GT(west->easting, 750000.0);
    const auto back = fromUtm(*west);
    ASSERT_TRUE(back.has_value());
    EXPECT_NEAR(back->longitudeDeg, 6.01, 1e-9);
}

TEST(GeoUtm, FormatsLikeTheSpecification) {
    const UtmCoordinate utm{30, 'T', true, 440123.44, 4474567.81};
    EXPECT_EQ(formatUtm(utm), "30T 440 123.4 E  4 474 567.8 N");
    const UtmCoordinate small{31, 'N', true, 500000.0, 12.34};
    EXPECT_EQ(formatUtm(small), "31N 500 000.0 E  12.3 N");
}

// --- Web Mercator (FR-GEO-02b) ------------------------------------------------------------------

TEST(GeoWebMercator, MatchesProjReference) {
    const CsvTable table("geo/web_mercator.csv");
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto p = toWebMercator(table.number(i, "lat_deg"), table.number(i, "lon_deg"));
        EXPECT_NEAR(p.x, table.number(i, "x_m"), kMillimetre);
        EXPECT_NEAR(p.y, table.number(i, "y_m"), kMillimetre);
        const LatLon back = fromWebMercator(p);
        EXPECT_NEAR(back.latitudeDeg, table.number(i, "lat_deg"), 1e-9);
        EXPECT_NEAR(back.longitudeDeg, table.number(i, "lon_deg"), 1e-9);
    }
}

TEST(GeoWebMercator, TilesFollowTheXyzScheme) {
    const auto world = tileBounds({0, 0, 0});
    ASSERT_TRUE(world.has_value());
    EXPECT_DOUBLE_EQ(world->west, -180.0);
    EXPECT_DOUBLE_EQ(world->east, 180.0);
    EXPECT_NEAR(world->north, kWebMercatorMaxLatitudeDeg, 1e-12);
    EXPECT_NEAR(world->south, -kWebMercatorMaxLatitudeDeg, 1e-12);

    // Zoom 1: four tiles, x grows east, y grows south.
    EXPECT_EQ(*tileAt(10.0, -10.0, 1), (TileId{0, 0, 1}));
    EXPECT_EQ(*tileAt(10.0, 10.0, 1), (TileId{1, 0, 1}));
    EXPECT_EQ(*tileAt(-10.0, -10.0, 1), (TileId{0, 1, 1}));
    // Clamping at the edges of the map.
    EXPECT_EQ(*tileAt(89.0, 180.0, 3), (TileId{0, 0, 3}));
    EXPECT_EQ(*tileAt(-89.0, 179.999999, 3), (TileId{7, 7, 3}));
}

TEST(GeoWebMercator, PointLiesInsideItsTileBounds) {
    for (const int zoom : {0, 5, 12, 18, 24}) {
        for (const LatLon p : {LatLon{51.5074, -0.1278}, LatLon{-33.8688, 151.2093},
                               LatLon{0.0, 0.0}, LatLon{64.1466, -21.9426}}) {
            const auto tile = tileAt(p.latitudeDeg, p.longitudeDeg, zoom);
            ASSERT_TRUE(tile.has_value());
            const auto bounds = tileBounds(*tile);
            ASSERT_TRUE(bounds.has_value());
            EXPECT_LE(bounds->west, p.longitudeDeg);
            EXPECT_GE(bounds->east, p.longitudeDeg);
            EXPECT_LE(bounds->south, p.latitudeDeg);
            EXPECT_GE(bounds->north, p.latitudeDeg);
        }
    }
}

TEST(GeoWebMercator, RejectsInvalidZoomAndTiles) {
    EXPECT_FALSE(tileAt(0.0, 0.0, -1).has_value());
    EXPECT_FALSE(tileAt(0.0, 0.0, kMaxTileZoom + 1).has_value());
    EXPECT_FALSE(tileBounds({2, 0, 1}).has_value());
    EXPECT_FALSE(tileBounds({0, -1, 1}).has_value());
}

TEST(GeoWebMercator, GroundResolution) {
    // 156 543.03 m/px at zoom 0 on the equator for 256-px tiles.
    EXPECT_NEAR(groundResolution(0.0, 0, 256), 156543.03392804097, 1e-6);
    EXPECT_NEAR(groundResolution(60.0, 1, 256), 156543.03392804097 / 4.0, 1e-6);
}

// --- Distance and bearing (FR-GEO-04) -----------------------------------------------------------

TEST(GeoGeodesic, MatchesGeographicLibWithinOneMillimetre) {
    const CsvTable table("geo/geodesic.csv");
    ASSERT_GT(table.size(), 150U);
    for (std::size_t i = 0; i < table.size(); ++i) {
        const LatLon a{table.number(i, "lat1_deg"), table.number(i, "lon1_deg")};
        const LatLon b{table.number(i, "lat2_deg"), table.number(i, "lon2_deg")};
        const GeodesicInverse r = inverseGeodesic(a, b);
        ASSERT_FALSE(r.approximate) << "row " << i;
        EXPECT_NEAR(r.distanceM, table.number(i, "distance_m"), kMillimetre) << "row " << i;
        if (r.distanceM > 1.0) {
            EXPECT_NEAR(
                ds::common::wrapDegrees180(r.initialBearingDeg - table.number(i, "azi1_deg")), 0.0,
                1e-6)
                << "row " << i;
            EXPECT_NEAR(ds::common::wrapDegrees180(r.finalBearingDeg - table.number(i, "azi2_deg")),
                        0.0, 1e-6)
                << "row " << i;
        }
    }
}

TEST(GeoGeodesic, CoincidentPointsGiveZero) {
    const GeodesicInverse r = inverseGeodesic({40.0, -3.0}, {40.0, -3.0});
    EXPECT_DOUBLE_EQ(r.distanceM, 0.0);
    EXPECT_FALSE(r.approximate);
}

TEST(GeoGeodesic, AntipodalPointsFallBackToSphere) {
    const GeodesicInverse r = inverseGeodesic({0.0, 0.0}, {0.5, 179.7});
    EXPECT_TRUE(r.approximate);
    EXPECT_NEAR(r.distanceM, 20003931.0, 0.01 * 20003931.0);
}

TEST(GeoLocalBearing, CompassDirections) {
    EXPECT_NEAR(localBearing({0, 0, 0}, {0, 10, 0}).bearingDeg, 0.0, 1e-12);
    EXPECT_NEAR(localBearing({0, 0, 0}, {10, 0, 0}).bearingDeg, 90.0, 1e-12);
    EXPECT_NEAR(localBearing({0, 0, 0}, {0, -10, 0}).bearingDeg, 180.0, 1e-12);
    EXPECT_NEAR(localBearing({0, 0, 0}, {-10, 0, 0}).bearingDeg, 270.0, 1e-12);
    EXPECT_NEAR(localBearing({1, 1, 5}, {4, 5, 0}).distanceM, 5.0, 1e-12);
}

}  // namespace
