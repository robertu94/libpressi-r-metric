#include <mutex>
#include <string>
#include "pressio_compressor.h"
#include "libpressio_ext/cpp/metrics.h"
#include "libpressio_ext/cpp/pressio.h"
#include "libpressio_ext/cpp/options.h"
#include "std_compat/utility.h"
#include "std_compat/optional.h"
#include "std_compat/memory.h"
#include "RInside.h"

extern "C" void libpressio_register_r_metric() {
}

std::mutex rmanager_global_lock;
/**
 * used to manage the lifetime of RInside's global instance
 * if instancePtr() == nullptr, then some other user has instantiated
 * RInside and we don't want to maintain the lifetime.
 *
 * If it is not nullptr, we need to keep this object alive until
 * the last reference to this object has been destroyed.
 */
struct rinside_manager {
  rinside_manager(): inside() {}
  rinside_manager(rinside_manager const&)=delete;
  rinside_manager& operator= (rinside_manager const&)=delete;
  rinside_manager(rinside_manager&& rhs)=delete;
  rinside_manager& operator=(rinside_manager&& rhs)=delete;

  static std::shared_ptr<rinside_manager> get_library() {
    static auto library = std::make_shared<rinside_manager>();
    return library;
  }

  RInside& instance() {
    return RInside::instance();
  }

  RInside inside;
};

Rcpp::NumericVector data_to_R(const pressio_data* const ptr) {
  auto dims = ptr->dimensions();
  auto data = ptr->to_vector<double>();
  Rcpp::NumericVector data_r = Rcpp::wrap(std::move(data));

  if(dims.size() >= 2) {
    data_r.attr("dim") = Rcpp::NumericVector(dims.begin(), dims.end());
  }
  
  return data_r;
}

template <class NumberVector, class NativeType>
pressio_option option_from_R_number_vector(Rcpp::RObject const& obj) {
    NumberVector const& v = Rcpp::as<NumberVector>(obj);
    if(v.length() == 0) {
      return pressio_option();
    } else if(v.length() == 1) {
      return pressio_option(v[0]);
    } else {
      if (v.hasAttribute("dim")) {
        auto r_dim = Rcpp::as<std::vector<int>>(obj.attr("dim"));
        std::vector<size_t> dim(r_dim.begin(), r_dim.end());
        std::vector<NativeType> data(v.begin(), v.end());
        return pressio_data::copy(
            pressio_dtype_from_type<NativeType>(),
            data.data(),
            dim
            );
      } else {
        return pressio_data(v.begin(), v.end());
      }
    }
}
pressio_option option_from_R_impl(Rcpp::RObject const& obj) {
  if(Rcpp::is<Rcpp::NumericVector>(obj)) {
    return option_from_R_number_vector<Rcpp::NumericVector, double>(obj);
  } else if(Rcpp::is<Rcpp::IntegerVector>(obj)) {
    return option_from_R_number_vector<Rcpp::IntegerVector, int>(obj);
  } else if(Rcpp::is<Rcpp::CharacterVector>(obj)) {
    Rcpp::CharacterVector const& v = Rcpp::as<Rcpp::CharacterVector>(obj);
    if(v.length() == 0) {
      return pressio_option();
    } else if(v.length() == 1) {
      return Rcpp::as<std::string>(v.at(0));
    } else {
      auto strings = Rcpp::as<std::vector<std::string>>(v);
      return strings;
    }
  } else if (obj.isNULL()) {
    return pressio_option();
  } else {
    throw std::runtime_error("type not supported ");
  }
}

class rcpp_metrics_plugin : public libpressio_metrics_plugin {
  public:

    rcpp_metrics_plugin(std::shared_ptr<rinside_manager>&& inside_mgr): inside_mgr(inside_mgr) {}

  int end_compress_impl(const struct pressio_data* uncompressed, const struct pressio_data*, int) override {
    if(!use_many) {
      data_uncompressed.resize(1);
      data_uncompressed[0] = pressio_data::clone(*uncompressed);
    }
    return 0;
  }
  int end_decompress_impl(const struct pressio_data*, const struct pressio_data* decompressed, int) override {
    if(!use_many) {
      compat::span<const pressio_data* const> outs(&decompressed, 1);
      run_r(outs);
    }
    return 0;
  }

  int end_compress_many_impl(compat::span<const pressio_data* const> const& data_inputs,
                                   compat::span<const pressio_data* const> const&, int ) override {
    if(use_many) {
      data_uncompressed.clear();
      data_uncompressed.reserve(data_inputs.size());
      for (size_t i = 0; i < data_inputs.size(); ++i) {
        data_uncompressed.emplace_back(pressio_data::clone(*data_inputs[i]));
      }
    }
    return 0;
  }
  int end_decompress_many_impl(compat::span<const pressio_data* const> const& ,
                                   compat::span<const pressio_data* const> const& data_outputs, int ) override {
    if(use_many) {
      run_r(data_outputs);
    }
    return 0;
  }

  void run_r(compat::span<const pressio_data* const> data_decompressed) {
    std::lock_guard<std::mutex> guard(rmanager_global_lock);
    RInside& R = inside_mgr->instance();
    metrics_results.clear();
    try {
      R.parseEvalQ("rm(list=ls())");
      for (size_t i = 0; i < data_uncompressed.size(); ++i) {
        auto name = std::string("in") + std::to_string(i);
        R[name] = data_to_R(&data_uncompressed[i]);
      }
      for (size_t i = 0; i < data_uncompressed.size(); ++i) {
        auto name = std::string("out") + std::to_string(i);
        R[name] = data_to_R(data_decompressed[i]);
      }
      R.parseEvalQ(script);

      for (auto const& i : this->outputs) {
        auto binding = R[i];
        if(not binding.exists()) { throw std::runtime_error("binding for " + i + " does not exist"); }
        pressio_option option = option_from_R_impl(binding);
        set(metrics_results, "rcpp:results:" + i, option);
      }

      set(metrics_results, "rcpp:error", std::string(""));
    } catch(std::exception const& e) {
      set(metrics_results, "rcpp:error", std::string(e.what()));
    }
  }

  struct pressio_options get_metrics_results(pressio_options const& ) override {
    return metrics_results;
  }

  pressio_options get_documentation_impl() const override {
    pressio_options opts;
    set(opts, "pressio:description", "Runs a R program to compute a metric");
    set(opts, "rcpp:outputs", "names of variables to pull from the R environment");
    set(opts, "rcpp:script", "script to evaluate with R");
    set(opts, "rcpp:use_many", "run the metrics with the _many versions of compress and decompress");
    return opts;
  }

  struct pressio_options get_configuration_impl() const override {
    pressio_options opts;
    set(opts, "pressio:thread_safe", pressio_thread_safety_single);
    set(opts, "pressio:stability", "external");
    
    std::vector<std::string> invalidations {}; 
    std::vector<pressio_configurable const*> invalidation_children {}; 
    
    set(opts, "predictors:requires_decompress", true);
    set(opts, "predictors:error_dependent", get_accumulate_configuration("predictors:error_dependent", invalidation_children, invalidations));
    set(opts, "predictors:error_agnostic", get_accumulate_configuration("predictors:error_agnostic", invalidation_children, invalidations));
    set(opts, "predictors:runtime", get_accumulate_configuration("predictors:runtime", invalidation_children, invalidations));

    return opts;
  }

  struct pressio_options get_options() const override {
    pressio_options opts;
    set(opts, "rcpp:outputs", outputs);
    set(opts, "rcpp:script", script);
    set(opts, "rcpp:use_many", use_many);
    return opts;
  }

  int set_options(pressio_options const& options) override {
    get(options, "rcpp:outputs", &outputs);
    get(options, "rcpp:script", &script);
    get(options, "rcpp:use_many", &use_many);
    return 0;
  }

  std::unique_ptr<libpressio_metrics_plugin> clone() override {
    return compat::make_unique<rcpp_metrics_plugin>(*this);
  }
  const char* prefix() const override {
    return "rcpp";
  }

  std::shared_ptr<rinside_manager> inside_mgr;
  int use_many = 0;
  std::string script;
  std::vector<std::string> outputs;
  std::vector<pressio_data> data_uncompressed;
  pressio_options metrics_results;
};

static pressio_register metrics_rcpp_plugin(metrics_plugins(), "rcpp", [](){
    return compat::make_unique<rcpp_metrics_plugin>(rinside_manager::get_library()); 
});
