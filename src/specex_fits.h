#ifndef SPECEX_FITS__H
#define SPECEX_FITS__H

namespace specex {

  class image_data;
  
  class FitsColumnDescription {
  public :
    int col;

    std::string unit;
    std::string format;
    std::vector<int> dimension;
    
    int SizeOfVectorOfDouble() const;
    bool IsDouble() const;
    
  };
  
  class FitsTableEntry {
  public :    
    std::string string_val;           // can be empty
    unbls::vector_double double_vals; // can be empty
    unbls::vector_int       int_vals; // can be empty
  };
  
  class FitsTable {
    
  public :
  
    std::map<std::string,FitsColumnDescription> columns;
    std::vector< std::vector<FitsTableEntry> > data;
     
    FitsTable();
    
    std::vector<int> decode_dimension(const std::string& tdim) const;
    std::string encode_dimension(const std::vector<int>& dimension) const;

    void AddColumnDescription(const std::string& ttype, const std::string& tform,  const std::string& tdim="", const std::string& tunit="");
  };

}

#endif
