#include "LaneSection.h"
#include "Road.h"

#include <iterator>
#include <stdexcept>
#include <utility>

namespace odr
{
LaneSection::LaneSection(double s0) : s0(s0) {}

ConstLaneSet LaneSection::get_lanes() const
{
    ConstLaneSet lanes;
    for (const auto& id_lane : this->id_to_lane)
        lanes.insert(id_lane.second);

    return lanes;
}

LaneSet LaneSection::get_lanes()
{
    LaneSet lanes;
    for (const auto& id_lane : this->id_to_lane)
        lanes.insert(id_lane.second);

    return lanes;
}

std::map<int, std::pair<Line3D, Line3D>> LaneSection::get_lane_border_lines(double resolution, bool with_lateralProfile, bool with_laneHeight) const
{
    if (auto road_ptr = this->road.lock())
    {
        auto s0_lanesec_iter = road_ptr->s0_to_lanesection.find(this->s0);
        if (s0_lanesec_iter == road_ptr->s0_to_lanesection.end())
            throw std::runtime_error("road associated with wrong lane section");

        const bool   is_last = (s0_lanesec_iter == std::prev(road_ptr->s0_to_lanesection.end()));
        const double next_s0 = is_last ? road_ptr->length : std::next(s0_lanesec_iter)->first;
        const double lanesec_len = next_s0 - this->s0;

        std::vector<double> s_vals;
        for (double s = this->s0; s < this->s0 + lanesec_len; s += resolution)
            s_vals.push_back(s);
        s_vals.push_back(next_s0 - (1e-9));

        std::map<int, std::pair<Line3D, Line3D>> lane_id_to_outer_inner_brdr_line;
        for (const double& s : s_vals)
        {
            for(const auto& id_lane : this->id_to_lane)
            {
                const int lane_id = id_lane.first;
                if (lane_id == 0)
                    continue;

                const double t_outer_brdr = id_lane.second->outer_border.get(s);
                const double t_inner_brdr = id_lane.second->inner_border.get(s);

                double z_inner_brdr = 0.0;
                double z_outer_brdr = 0.0;
                if (with_lateralProfile)
                {
                    z_inner_brdr = -std::tan(road_ptr->crossfall.get_crossfall(s, (lane_id > 0))) * std::abs(t_inner_brdr);
                    if (this->id_to_lane.at(lane_id)->level)
                    {
                        const double superelev = road_ptr->superelevation.get(s); // cancel out superelevation
                        z_outer_brdr = z_inner_brdr + std::tan(superelev) * (t_outer_brdr - t_inner_brdr);
                    }
                    else
                    {
                        z_outer_brdr = -std::tan(road_ptr->crossfall.get_crossfall(s, (lane_id > 0))) * std::abs(t_outer_brdr);
                    }
                }

                if (with_laneHeight && this->id_to_lane.at(lane_id)->s0_to_height_offset.size() > 0)
                {
                    const std::map<double, HeightOffset>& height_offs = this->id_to_lane.at(lane_id)->s0_to_height_offset;
                    auto                                  s0_height_offs_iter = height_offs.upper_bound(s - this->s0);
                    if (s0_height_offs_iter != height_offs.begin())
                        s0_height_offs_iter--;

                    const double inner_height = s0_height_offs_iter->second.inner;
                    z_inner_brdr += inner_height;
                    const double outer_height = s0_height_offs_iter->second.outer;
                    z_outer_brdr += outer_height;

                    if (std::next(s0_height_offs_iter) != height_offs.end())
                    {
                        /* if successive lane height entry available linearly interpolate */
                        const double ds = std::next(s0_height_offs_iter)->first - s0_height_offs_iter->first;
                        const double dh_inner = std::next(s0_height_offs_iter)->second.inner - inner_height;
                        z_inner_brdr += (dh_inner / ds) * (s - this->s0 - s0_height_offs_iter->first);
                        const double dh_outer = std::next(s0_height_offs_iter)->second.outer - outer_height;
                        z_outer_brdr += (dh_outer / ds) * (s - this->s0 - s0_height_offs_iter->first);
                    }
                }
                lane_id_to_outer_inner_brdr_line[lane_id].first.push_back(road_ptr->get_xyz(s, t_outer_brdr, z_outer_brdr, with_lateralProfile));
                lane_id_to_outer_inner_brdr_line[lane_id].second.push_back(road_ptr->get_xyz(s, t_inner_brdr, z_inner_brdr, with_lateralProfile));
            }
        }

        return lane_id_to_outer_inner_brdr_line;
    }
    else
    {
        throw std::runtime_error("could not access parent road for lane section");
    }

    return {};
}

std::vector<LaneVertices> LaneSection::get_lane_vertices(double resolution, bool with_lateralProfile, bool with_laneHeight) const
{
    std::vector<LaneVertices>                lanesection_vertices;
    std::map<int, std::pair<Line3D, Line3D>> lane_id_to_outer_inner_brdr_line =
        this->get_lane_border_lines(resolution, with_lateralProfile, with_laneHeight);

    for (auto& id_outer_inner_brdr_line : lane_id_to_outer_inner_brdr_line)
    {
        const int    lane_id = id_outer_inner_brdr_line.first;
        LaneVertices lane_vertices;
        lane_vertices.lane_id = lane_id;
        lane_vertices.type = this->id_to_lane.at(lane_id)->type;

        Line3D& outer_brdr_line = id_outer_inner_brdr_line.second.first;
        Line3D& inner_brdr_line = id_outer_inner_brdr_line.second.second;
        if (outer_brdr_line.size() != inner_brdr_line.size())
            throw std::runtime_error("outer and inner border line should have equal number of points");

        lane_vertices.vertices = outer_brdr_line;
        lane_vertices.vertices.insert(lane_vertices.vertices.end(), inner_brdr_line.rbegin(), inner_brdr_line.rend());

        const size_t num_pts = lane_vertices.vertices.size();
        for (size_t l_idx = 1, r_idx = num_pts - 2; l_idx < (num_pts >> 1); l_idx++, r_idx--)
        {
            std::vector<size_t> indicies_patch;
            if (lane_id > 0) // make sure triangle normal is facing "up"
                indicies_patch = {l_idx, l_idx - 1, r_idx + 1, r_idx, l_idx, r_idx + 1};
            else
                indicies_patch = {l_idx, r_idx + 1, l_idx - 1, r_idx, r_idx + 1, l_idx};
            lane_vertices.indices.insert(lane_vertices.indices.end(), indicies_patch.begin(), indicies_patch.end());
        }

        lanesection_vertices.push_back(lane_vertices);
    }

    return lanesection_vertices;
}

} // namespace odr