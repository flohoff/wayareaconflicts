#include <iostream>
#include <getopt.h>
#include <unordered_map>
#include <string>
#include <list>
#include <set>

#include <google/sparsetable>

#include <osmium/io/any_input.hpp>
#include <osmium/handler.hpp>
#include <osmium/visitor.hpp>
#include <osmium/handler/dump.hpp>
#include <osmium/handler/dump.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/all.hpp>
#include <osmium/geom/ogr.hpp>
#include <osmium/geom/factory.hpp>
#include <osmium/osm/timestamp.hpp>

#include <boost/program_options.hpp>

#define DEBUG 0

#include <gdalcpp.hpp>

class AttachedWay;

const std::vector<std::string>	areatags = { "landuse", "landcover", "amenity", "leisure", "natural", "building" };
const std::vector<std::string>	possibleareatags = { "highway", "aeroway", "waterway", "power", "railway" };

typedef std::vector<AttachedWay *> waycontainer_t;
typedef std::pair<osmium::Location, waycontainer_t *> nodeinfo_t;
typedef osmium::index::map::SparseMemTable<osmium::unsigned_object_id_type, nodeinfo_t *> nodeindex_t;

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

	AttachedWay() : wayisarea(WAY_IS_UNKNOWN) {
	}

	AttachedWay(const osmium::Way& way) {
		wayid=way.id();
		username=strdup(way.user());
		timestamp=way.timestamp();
		changesetid=way.changeset();

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

		if (taglist.size() == 0) {
			wayisarea=WAY_IS_UNKNOWN;
			return;
		}

		/* Walk a list of default area tags landuse, landcover, leisure, amenity, building */
		for(auto t : areatags) {
			if (taglist.get_value_by_key(t.c_str())) {
				ptag=strdup(t.c_str());
				wayisarea=WAY_IS_AREA;
				return;
			}
		}

		for(auto t : possibleareatags) {
			if (!taglist.get_value_by_key(t.c_str()))
				continue;

			if (!way.is_closed()) {
				ptag=strdup(t.c_str());
				wayisarea=WAY_IS_LINE;
				return;
			}

			if (tagvalue_is_true(taglist, "area")) {
				ptag=strdup(t.c_str());
				wayisarea=WAY_IS_AREA;
				return;
			}
		}

		if (!way.is_closed()) {
			wayisarea=WAY_IS_UNKNOWN;
			return;
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

		} catch(osmium::invalid_location) {
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

                feature.add_to_layer();
        }
};

struct AttachedWaysHandler : public osmium::handler::Handler {
	std::vector<AttachedWay*>		attachedways;
	nodeindex_t				*nindex;
	osmium::geom::OGRFactory<>		ogrfactory {};

	AttachedWaysHandler(nodeindex_t *enindex) {
		nindex=enindex;
	}

	int attachedways_num() {
		return attachedways.size();
	}

	void checkadjacent(AttachedWay *awo, osmium::object_id_type nid1, osmium::object_id_type nid2, MyOGRHandler &writer) {
		waycontainer_t	*ways=nindex->get(nid1)->second;
		// FIXME - Non existant node
		for(auto aw : *ways) {
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

			osmium::Location	nl1=nindex->get(nid1)->first,
						nl2=nindex->get(nid2)->first;

			writer.write_line(nl1, nl2, awo, aw);
		}
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

				if (DEBUG)
						std::cout << "\tNode: " << nid 
						<< " count: " << nindex->get(nid)->second->size()
						<< " Last: " << lastnid
						<< std::endl;

				if (nindex->get(nid)->second->size() <= 1) {
					lastnid=0;
					continue;
				}

				if (lastnid) {
					/*
					 * Check other ways connected to THIS node if they are also attached
					 * to the previous node
					 */
					checkadjacent(aw, nid, lastnid, writer);
				}
				lastnid=nid;
			}

			if (DEBUG)
				std::cout << std::endl;
		}
	}


	void node(const osmium::Node& node) {
		nodeinfo_t		*ni;
		osmium::object_id_type	nid=node.id();

		try {
			ni=nindex->get(nid);
		} catch (osmium::not_found&) {
			ni=new nodeinfo_t;
			nindex->set(nid, ni);
			ni->first=node.location();
		}
	}

	void way_add_unique(AttachedWay *aw, waycontainer_t *wc) {
		for(auto awi : *wc) {
			if (awi->id() == aw->id()) {
				return;
			}
		}
		wc->push_back(aw);
	}

	void way(const osmium::Way& way) {

		AttachedWay *aw=new AttachedWay(way);
		attachedways.push_back(aw);

		for (auto& node_ref : way.nodes()) {
			nodeinfo_t		*ni;
			osmium::object_id_type	nid=node_ref.ref();

			if (DEBUG)
				std::cout << "Adding way " << way.id() << " Nodeid: " << nid << std::endl;

			try {
				ni=nindex->get(nid);
			} catch (osmium::not_found&) {
				ni=new nodeinfo_t;
				nindex->set(nid, ni);
			}

			if (!ni->second) {
				ni->second=new waycontainer_t;
				ni->second->push_back(aw);
				continue;
			}

			way_add_unique(aw, ni->second);
		}
	}
};

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
	std::string	directory=".";

        po::options_description         desc("Allowed options");
        desc.add_options()
                ("help,h", "produce help message")
                ("infile,i", po::value<std::string>(), "Input file")
		("dbname", po::value<std::string>(), "Output database name")
        ;
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	nodeindex_t		ni;
	OGRRegisterAll();

	gdalcpp::Dataset	dataset{"sqlite", vm["dbname"].as<std::string>(),
			gdalcpp::SRS{}, { "SPATIALITE=TRUE", "INIT_WITH_EPSG=no" }};

	dataset.exec("PRAGMA synchronous = OFF");

	osmium::geom::OGRFactory<>	ogrfactory {};

	osmium::io::File	infile(vm["infile"].as<std::string>());
	osmium::io::Reader	reader(infile);

	AttachedWaysHandler	awhandler(&ni);

	osmium::apply(reader, awhandler);
	reader.close();

	MyOGRHandler		lwriter{dataset, ogrfactory};

	awhandler.finddoubles(lwriter);

	OGRCleanupAll();
	exit(0);
}
