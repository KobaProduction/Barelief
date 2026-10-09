#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// GUI-owned immutable projection, never a borrowed processing buffer. A
// transposed view shares storage; only indexing changes, not sample order.
class NumericMatrixSnapshot {
public:
    using Matrix=std::vector<std::vector<float>>;
    class Row {
    public:
        size_t size() const noexcept {return matrix_->transposed_?matrix_->storage_->size():matrix_->storage_->front().size();}
        bool empty() const noexcept {return size()==0;}
        float operator[](size_t column) const noexcept {
            return matrix_->transposed_?(*matrix_->storage_)[column][row_]:(*matrix_->storage_)[row_][column];
        }
    private:
        friend class NumericMatrixSnapshot;
        Row(const NumericMatrixSnapshot* matrix,size_t row):matrix_(matrix),row_(row){}
        const NumericMatrixSnapshot* matrix_;size_t row_;
    };
    NumericMatrixSnapshot()=default;
    static NumericMatrixSnapshot take(Matrix rows) {
        const auto columns=rows.empty()?0:rows.front().size();
        if(rows.size()>16384 || columns>16384 || (columns && rows.size()>1048576/columns))return {};
        for(const auto& row:rows) {
            if(row.size()!=columns)return {};
            for(float value:row)if(!std::isfinite(value))return {};
        }
        if(rows.empty() || !columns)return {};
        NumericMatrixSnapshot result;result.storage_=std::make_shared<const Matrix>(std::move(rows));return result;
    }
    NumericMatrixSnapshot transposed() const noexcept {auto result=*this;result.transposed_=!result.transposed_;return result;}
    bool empty() const noexcept {return !storage_;}
    size_t size() const noexcept {return empty()?0:transposed_?storage_->front().size():storage_->size();}
    Row operator[](size_t row) const noexcept {return {this,row};}
    Row front() const noexcept {return (*this)[0];}
    void clear() noexcept {*this={};}
    const void* storage_identity() const noexcept {return storage_.get();}
    size_t sample_count() const noexcept {return empty()?0:size()*front().size();}
    bool operator==(const NumericMatrixSnapshot& other) const noexcept {
        if(storage_==other.storage_ && transposed_==other.transposed_)return true;
        if(size()!=other.size() || (!empty() && front().size()!=other.front().size()))return false;
        for(size_t row=0;row<size();++row)for(size_t column=0;column<front().size();++column)
            if((*this)[row][column]!=other[row][column])return false;
        return true;
    }
private:
    std::shared_ptr<const Matrix> storage_;
    bool transposed_=false;
};
