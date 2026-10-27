#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <osmium/io/any_input.hpp>
#include <osmium/handler.hpp>
#include <osmium/visitor.hpp>
#include <osmium/geom/ogr.hpp>
#include <osmium/osm/timestamp.hpp>

#include <boost/program_options.hpp>

#define DEBUG 0

#include <gdalcpp.hpp>

class AttachedWay;

const std::vector<std::string>	areatags = { "landuse", "landcover", "amenity", "leisure", "natural", "building" };
const std::vector<std::string>	possibleareatags = { "highway", "aeroway", "waterway", "power", "railway" };

typedef std::vector<AttachedWay *> waycontainer_t;
typedef std::pair<osmium::unsigned_object_id_type, osmium::Location> locpair_t;
typedef std::pair<osmium::unsigned_object_id_type, AttachedWay *> wayref_t;

enum {
	WAY_IS_UNKNOWN,
	WAY_IS_LINE,
	WAY_IS_AREA
};

class AttachedWay {

	char					wayisarea;
	const char				*ptag;
	const char				*username;
	osmium::Timestamp			timestamp;
	osmium::object_id_type			wayid;
	osmium::object_id_type			changesetid;
public:

	std::vector<osmium::object_id_type>	nodeids;

	AttachedWay() : wayisarea(WAY_IS_UNKNOWN), ptag(nullptr) {
	}

	AttachedWay(const osmium::Way& way) : ptag(nullptr) {
		wayid=way.id();
		username=strdup(way.user());
		timestamp=way.timestamp();
		changesetid=way.changeset();

		nodeids.reserve(way.nodes().size());
		for (auto& node_ref : way.nodes()) {
			nodeids.push_back(node_ref.ref());
		}

		wayarea(way);
	}

	char area(void ) {
		return wayisarea;
	}

	osmium::Timestamp time(void ) {
		return timestamp;
	}

	const char *user() {
		return username;
	}

	const char *primarytag(void ){
		return ptag;
	}

	bool tagvalue_is_true(const osmium::TagList& taglist, const char *k) {
		const char *v=taglist.get_value_by_key(k);
		if (!v)
			return false;

		if (!strcmp(v, "yes") || !strcmp(v, "true"))
			return true;

		return false;
	}

	bool mayoverlap(AttachedWay *aw) {
		if (area() == WAY_IS_UNKNOWN || aw->area() == WAY_IS_UNKNOWN)
			return true;

		if (area() == WAY_IS_LINE && aw->area() == WAY_IS_AREA)
			return false;

		if (area() == WAY_IS_AREA && aw->area() == WAY_IS_LINE)
			return false;

		return true;
	}

	void wayarea(const osmium::Way& way) {
		const osmium::TagList& taglist=way.tags();

		/* empty ways (e.g. action=delete) can neither be line nor area */
		if (taglist.size() == 0 || way.nodes().empty()) {
			wayisarea=WAY_IS_UNKNOWN;
			return;
		}

		/* Walk a list of default area tags landuse, landcover, leisure, amenity, building */
		/* ptag points into the const global tag vectors: stable for program lifetime */
		for(const auto& t : areatags) {
			if (taglist.get_value_by_key(t.c_str())) {
				ptag=t.c_str();
				wayisarea=WAY_IS_AREA;
				return;
			}
		}

		for(const auto& t : possibleareatags) {
			if (!taglist.get_value_by_key(t.c_str()))
				continue;

			if (!way.is_closed()) {
				ptag=t.c_str();
				wayisarea=WAY_IS_LINE;
				return;
			}

			if (tagvalue_is_true(taglist, "area")) {
				ptag=t.c_str();
				wayisarea=WAY_IS_AREA;
				return;
			}
		}

		wayisarea=WAY_IS_UNKNOWN;
		return;
	}

	/*
	 * Walk way nodes and check wether we have a segment made out of
	 * those 2 node ids in either direction
	 */
	bool hassegment(osmium::object_id_type n1, osmium::object_id_type n2) {
		osmium::object_id_type on2=0;

		for (auto on1 : nodeids) {

			if (DEBUG)
				std::cout << "\t\t\tn1 " << n1 << " n2 " << n2 << " on1 " << on1 << " on2 " << on2 << std::endl;

			if (on2) {
				if ((n1 == on1 && n2 == on2) ||
					(n2 == on1 && n1 == on2) ) {
					return true;
				}
			}

			on2=on1;
		}

		if ((nodeids.back() == n1 && nodeids[0] == n2) ||
			(nodeids.back() == n2 && nodeids[0] == n1)) {
			return true;
		}

		return false;
	}

	osmium::object_id_type id(void ) {
		return wayid;
	}

	osmium::object_id_type changeset(void ) {
		return changesetid;
	}

};


class MyOGRHandler : public osmium::handler::Handler {

    gdalcpp::Layer m_layer_linestring;

    osmium::geom::OGRFactory<>& m_factory;

public:

    explicit MyOGRHandler(gdalcpp::Dataset& dataset, osmium::geom::OGRFactory<>& factory) :
        m_layer_linestring(dataset, "wayareaconflict", wkbLineString),
	m_factory(factory) {
        m_layer_linestring.add_field("lineway_id", OFTString, 20);
        m_layer_linestring.add_field("lineway_type", OFTString, 20);
        m_layer_linestring.add_field("lineway_changeset", OFTString, 20);
        m_layer_linestring.add_field("areaway_id", OFTString, 20);
        m_layer_linestring.add_field("areaway_type", OFTString, 20);
        m_layer_linestring.add_field("areaway_changeset", OFTString, 20);
        m_layer_linestring.add_field("style", OFTString, 20);
	}

        void write_line(
                        const osmium::Location&		point1,
                        const osmium::Location&		point2,
			AttachedWay			*awo,
			AttachedWay			*aw)
                        {

		std::unique_ptr<OGRLineString> connection_line{new OGRLineString};

		try {
			std::unique_ptr<OGRPoint> op1=m_factory.create_point(point1);
			std::unique_ptr<OGRPoint> op2=m_factory.create_point(point2);

			connection_line->addPoint(op1.get());
			connection_line->addPoint(op2.get());

		} catch(const osmium::invalid_location&) {
			std::cerr << "Invalid location" << std::endl;
			return;
		}

		gdalcpp::Feature feature{m_layer_linestring, std::move(connection_line)};

                feature.set_field("lineway_id", static_cast<double>(awo->id()));
                feature.set_field("lineway_type", awo->primarytag());
                feature.set_field("lineway_changeset", static_cast<double>(awo->changeset()));
                feature.set_field("areaway_id" , static_cast<double>(aw->id()));
                feature.set_field("areaway_type", aw->primarytag());
                feature.set_field("areaway_changeset", static_cast<double>(aw->changeset()));

                feature.set_field("style", "redline");

                feature.add_to_layer();
        }
};

struct AttachedWaysHandler : public osmium::handler::Handler {
	std::vector<AttachedWay*>		attachedways;
	std::vector<locpair_t>			locations;
	std::vector<wayref_t>			wayrefs;
	std::unordered_map<osmium::unsigned_object_id_type, waycontainer_t *>	sharednodes;

	AttachedWaysHandler() {
	}

	void checkadjacent(
			AttachedWay *awo,
			waycontainer_t *wc,
			osmium::object_id_type nid1, osmium::object_id_type nid2,
			MyOGRHandler &writer) {
		for(auto aw : *wc) {
			if (DEBUG)
				std::cout << "\t\tChecking " << aw->id() << std::endl;

			if (aw->id() == awo->id())
				continue;

			if (!aw->hassegment(nid1, nid2)) {
				continue;
			}

			if (DEBUG)
				std::cout << "\t\tHas segment " << nid1 << " " << nid2 << std::endl;

			if (aw->mayoverlap(awo)) {
				continue;
			}

			std::cout << "overlap: "
				<< " " << awo->primarytag() << "," << aw->primarytag() << " "
				<< "w" << awo->id() << ",w" << aw->id()
				<< " n" << nid1 << ",n" << nid2 << " "
				<< " changesets " << awo->changeset() << "," << aw->changeset() << " ";

			osmium::Timestamp	t=aw->time();
			if (awo->time() > t) {
				std::cout << awo->time().to_iso() << " " << awo->user() << "\n";
			} else {
				std::cout << aw->time().to_iso() << " " << aw->user() << "\n";
			}

			osmium::Location	nl1=location_for(nid1),
						nl2=location_for(nid2);

			writer.write_line(nl1, nl2, awo, aw);
		}
	}

	/*
	 * Locations sorted by id; binary search. Returns an undefined
	 * Location for nodes we do not know.
	 */
	osmium::Location location_for(osmium::unsigned_object_id_type nid) const {
		const auto it=std::lower_bound(locations.begin(), locations.end(), nid,
				[](const locpair_t& p, osmium::unsigned_object_id_type id) { return p.first < id; });

		if (it != locations.end() && it->first == nid)
			return it->second;

		return osmium::Location{};
	}

	/*
	 * Sort what was collected blindly during the handler passes and
	 * build the way containers for all nodes referenced by more than
	 * one way. No lookups happen while collecting, so files with
	 * missing nodes or out of order objects are handled fine.
	 */
	void prepare(void) {
		std::sort(locations.begin(), locations.end(),
			  [](const locpair_t& a, const locpair_t& b) { return a.first < b.first; });
		/* pair operator< : id, then pointer - duplicate refs from closed
		   ways (first node == last node) end up adjacent */
		std::sort(wayrefs.begin(), wayrefs.end());

		for(auto it=wayrefs.begin(); it != wayrefs.end(); ) {
			auto run_end=it;
			while (run_end != wayrefs.end() && run_end->first == it->first)
				++run_end;

			if (run_end - it >= 2) {
				waycontainer_t	*wc=new waycontainer_t;
				for(auto r=it; r != run_end; ++r) {
					/* dedup: same way traversing the same node twice */
					if (wc->empty() || wc->back() != r->second)
						wc->push_back(r->second);
				}
				if (wc->size() > 1)
					sharednodes.emplace(it->first, wc);
				else
					delete wc;
			}

			it=run_end;
		}

		std::vector<wayref_t>().swap(wayrefs);
	}

	void finddoubles(MyOGRHandler &writer) {
		for(auto aw : attachedways) {
			osmium::object_id_type	lastnid=0;

			if (DEBUG)
				std::cout << "Way " << aw->id() << std::endl;

			/*
			 * Halve the runtime by only going from line -> area 
			 * and not processing the other way around
			 */
			if (aw->area() != WAY_IS_LINE)
				continue;

			/* Check if there are 2 consecutive nodes which have a refcount > 1 */
			for(auto nid : aw->nodeids) {
				auto			wit=sharednodes.find(nid);
				waycontainer_t		*wc=(wit == sharednodes.end()) ? nullptr : wit->second;
				if (DEBUG)
						std::cout << "\tNode: " << nid 
						<< " count: " << (wc ? wc->size() : 0)
						<< " Last: " << lastnid
						<< std::endl;

				if (!wc || wc->size() <= 1) {
					lastnid=0;
					continue;
				}

				if (lastnid) {
					/*
					 * Check other ways connected to THIS node if they are also attached
					 * to the previous node
					 */
					checkadjacent(aw, wc, nid, lastnid, writer);
				}

				lastnid=nid;
			}

			if (DEBUG)
				std::cout << std::endl;
		}
	}


	void node(const osmium::Node& node) {
		locations.emplace_back(node.id(), node.location());
	}

	void way(const osmium::Way& way) {

		AttachedWay *aw=new AttachedWay(way);
		attachedways.push_back(aw);

		for (auto& node_ref : way.nodes()) {
			osmium::object_id_type	nid=node_ref.ref();

			if (DEBUG)
				std::cout << "Adding way " << way.id() << " Nodeid: " << nid << std::endl;

			wayrefs.emplace_back(nid, aw);
		}
	}
};

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
	std::string	directory=".";

	po::options_description		desc("Allowed options");
	desc.add_options()
		("help,h", "produce help message")
		("infile,i", po::value<std::string>()->required(), "Input file")
		("dbname", po::value<std::string>()->required(), "Output database name")
	;
	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
	} catch(const boost::program_options::error& e) {
		std::cerr << "Error: " << e.what() << "\n";
		std::cout << desc << std::endl;
		exit(-1);
	}

	OGRRegisterAll();

	gdalcpp::Dataset	dataset{"sqlite", vm["dbname"].as<std::string>(),
			gdalcpp::SRS{}, { "SPATIALITE=TRUE", "INIT_WITH_EPSG=no" }};

	dataset.exec("PRAGMA synchronous = OFF");

	osmium::geom::OGRFactory<>	ogrfactory {};

	osmium::io::File	infile(vm["infile"].as<std::string>());
	osmium::io::Reader	reader(infile, osmium::osm_entity_bits::node | osmium::osm_entity_bits::way);

	AttachedWaysHandler	awhandler;

	osmium::apply(reader, awhandler);
	reader.close();

	awhandler.prepare();

	MyOGRHandler		lwriter{dataset, ogrfactory};

	awhandler.finddoubles(lwriter);

	OGRCleanupAll();
	exit(0);
}
