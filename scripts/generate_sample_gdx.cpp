#include "gdx.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
	const std::string output_path = argc > 1 ? argv[1] : "sample.gdx";

	std::string error_message;
	gdx::TGXFileObj gdx_file(error_message);
	if (!error_message.empty()) {
		std::cerr << "Failed to load GDX runtime: " << error_message << std::endl;
		return 1;
	}

	int err_nr = 0;
	if (!gdx_file.gdxOpenWrite(output_path.c_str(), "gdx_fixture", err_nr)) {
		std::cerr << "Failed to open GDX file for writing: error " << err_nr << std::endl;
		return 1;
	}

	auto write_set = [&](const char *name, const char *desc, const std::vector<std::string> &elements) {
		gdxStrIndex_t index{};
		gdxStrIndexPtrs_t ptrs{};
		GDXSTRINDEXPTRS_INIT(index, ptrs);
		gdxValues_t values{};
		if (!gdx_file.gdxDataWriteStrStart(name, desc, 1, GMS_DT_SET, 0)) {
			throw std::runtime_error(std::string("Failed to start writing set ") + name);
		}
		for (const auto &elem : elements) {
			std::strncpy(index[0], elem.c_str(), GMS_SSSIZE - 1);
			index[0][GMS_SSSIZE - 1] = '\0';
			values[GMS_VAL_LEVEL] = 1.0;
			if (!gdx_file.gdxDataWriteStr(ptrs, values)) {
				throw std::runtime_error(std::string("Failed to write element for set ") + name);
			}
		}
		if (!gdx_file.gdxDataWriteDone()) {
			throw std::runtime_error(std::string("Failed to finalize set ") + name);
		}
	};

	auto write_parameter = [&](const char *name, const char *desc, int dim,
	                           const std::vector<std::vector<std::string>> &keys,
	                           const std::vector<double> &levels) {
		gdxStrIndex_t index{};
		gdxStrIndexPtrs_t ptrs{};
		GDXSTRINDEXPTRS_INIT(index, ptrs);
		gdxValues_t values{};
		if (!gdx_file.gdxDataWriteStrStart(name, desc, dim, GMS_DT_PAR, 0)) {
			throw std::runtime_error(std::string("Failed to start writing parameter ") + name);
		}
		for (size_t i = 0; i < keys.size(); ++i) {
			for (int d = 0; d < dim; ++d) {
				std::strncpy(index[d], keys[i][d].c_str(), GMS_SSSIZE - 1);
				index[d][GMS_SSSIZE - 1] = '\0';
			}
			values[GMS_VAL_LEVEL] = levels[i];
			if (!gdx_file.gdxDataWriteStr(ptrs, values)) {
				throw std::runtime_error(std::string("Failed to write record for parameter ") + name);
			}
		}
		if (!gdx_file.gdxDataWriteDone()) {
			throw std::runtime_error(std::string("Failed to finalize parameter ") + name);
		}
	};

	try {
		write_set("cities", "Set of cities", {"New-York", "Chicago", "Topeka"});
		write_set("plants", "Set of plants", {"Seattle", "San-Diego"});

		write_parameter("demand", "City demand", 1,
		               {{"New-York"}, {"Chicago"}, {"Topeka"}},
		               {324.0, 299.0, 274.0});

		write_parameter("plant_capacity", "Plant capacity", 1,
		               {{"Seattle"}, {"San-Diego"}},
		               {350.0, 600.0});

		write_parameter("ship_cost", "Shipping cost", 2,
		               {{"Seattle", "New-York"}, {"Seattle", "Chicago"}, {"Seattle", "Topeka"},
		                {"San-Diego", "New-York"}, {"San-Diego", "Chicago"}, {"San-Diego", "Topeka"}},
		               {0.225, 0.153, 0.162, 0.225, 0.162, 0.126});
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		gdx_file.gdxClose();
		return 1;
	}

	if (gdx_file.gdxClose()) {
		std::cerr << "Failed to close GDX file" << std::endl;
		return 1;
	}

	return 0;
}
